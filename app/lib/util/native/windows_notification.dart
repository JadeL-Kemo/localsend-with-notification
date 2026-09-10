import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:localsend_app/gen/strings.g.dart';
import 'package:localsend_app/util/native/open_folder.dart';
import 'package:localsend_app/util/native/tray_helper.dart';
import 'package:window_manager/window_manager.dart';

/// Where a toast click should lead in Explorer.
/// - [folderPath] with non-null [fileName]: select that file/folder in Explorer.
/// - [folderPath] with null [fileName]: open the folder itself.
typedef ToastOpenTarget = ({String folderPath, String? fileName});

/// Additive Windows toast helper.
///
/// Loads `localsend_windows_notify.dll` (built from
/// `app/windows/windows_notification/`) via dart:ffi. The DLL is purely
/// additive: it is not referenced by the official runner, so the official
/// executable stays untouched.
///
/// All failures are swallowed (only logged): notifications must never
/// interfere with the receive flow.
class WindowsNotification {
  static const String _dllName = 'localsend_windows_notify.dll';

  /// AppUserModelID used for the Start Menu shortcut registered on first use.
  /// Must stay stable; toast click activation uses the shortcut target.
  static const String appId = 'LocalSend.Notifications';

  static WindowsNotification? _instance;
  static WindowsNotification get instance =>
      _instance ??= WindowsNotification._create();

  late final bool _loaded;
  late final ffi.DynamicLibrary? _lib;

  bool _identityEnsured = false;
  bool _activationAttached = false;

  /// Receives the toast `launch` payload when the user clicks a toast.
  ///
  /// Must stay referenced for the lifetime of the process: the native side
  /// calls into it from a WinRT thread.
  static final ffi.NativeCallable<ffi.Void Function(ffi.Pointer<Utf16>)> _activationCallable =
      ffi.NativeCallable<ffi.Void Function(ffi.Pointer<Utf16>)>.listener(_onNativeActivation);

  /// Pending "open this file in Explorer" action, consumed when the app is
  /// activated (single-instance handshake after a toast click).
  ToastOpenTarget? _pendingOpenTarget;
  bool _pendingHome = false;

  WindowsNotification._create() {
    if (!Platform.isWindows) {
      _loaded = false;
      return;
    }
    try {
      _lib = ffi.DynamicLibrary.open(_dllName);
      _loaded = true;
    } catch (e) {
      _lib = null;
      _loaded = false;
      debugPrint('[WindowsNotification] DLL load failed: $e');
    }
  }

  /// Whether toast notifications can be used on this machine
  /// (Windows 10+ and the DLL is present).
  bool get isSupported {
    if (!_loaded || _lib == null) {
      return false;
    }
    try {
      final fn = _lib.lookupFunction<ffi.Int32 Function(), int Function()>(
        'ls_toast_supported',
      );
      return fn() == 1;
    } catch (e) {
      debugPrint('[WindowsNotification] supported() failed: $e');
      return false;
    }
  }

  /// Registers (idempotently) the Start Menu shortcut with our AUMID.
  /// Should be called once before showing any toast.
  /// Returns the native result code (0 = ok, null = skipped/error).
  Future<int?> ensureRegistered() async {
    if (!Platform.isWindows || _lib == null) {
      return null;
    }
    final code = await _runInIsolate((lib) {
      final fn = lib.lookupFunction<
        ffi.Int32 Function(ffi.Pointer<ffi.Uint16>),
        int Function(ffi.Pointer<ffi.Uint16>)
      >('ls_toast_ensure_identity');
      return _withNativeString(appId, (idPtr) => fn(idPtr));
    });
    if (code != null && code != 0) {
      debugPrint('[WindowsNotification] ensure_identity failed (code $code)');
    }
    return code;
  }

  /// Shows a toast. Returns the native result code (0 = ok, null = skipped).
  Future<int?> show({
    required String title,
    required String body,
    String? tag,
    String? launch,
  }) async {
    if (!Platform.isWindows || _lib == null) {
      return null;
    }
    return _runInIsolate((lib) {
      final fn = lib.lookupFunction<
        ffi.Int32 Function(
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
        ),
        int Function(
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
          ffi.Pointer<ffi.Uint16>,
        )
      >('ls_toast_show');
      return _withNativeString(appId, (appIdPtr) {
        return _withNativeString(title, (titlePtr) {
          return _withNativeString(body, (bodyPtr) {
            return _withOptionalNativeString(tag, (tagPtr) {
              return _withOptionalNativeString(launch, (launchPtr) => fn(appIdPtr, titlePtr, bodyPtr, tagPtr, launchPtr));
            });
          });
        });
      });
    });
  }

  /// Sends a test toast. Called right after the user enables the setting, so a
  /// failure is visible immediately instead of only after a real transfer.
  /// Returns the native result code (0 = ok, null = skipped/error).
  Future<int?> sendTestToast() async {
    if (!isSupported) {
      debugPrint('[WindowsNotification] not supported, test toast skipped');
      return null;
    }
    await ensureRegisteredOnce();
    final code = await show(
      title: t.notificationToasts.appTitle,
      body: t.notificationToasts.enabledNotice,
      tag: 'test',
      launch: 'action=home',
    );
    debugPrint('[WindowsNotification] test toast result: $code');
    return code;
  }

  /// Registers the AUMID shortcut lazily (once per process).
  Future<void> ensureRegisteredOnce() async {
    if (_identityEnsured) {
      return;
    }
    await ensureRegistered();
    await attachActivationHandler();
    _identityEnsured = true;
  }

  /// Toast for a receive request (files, folder, text message or a mix).
  /// Clicking brings the app to the receive home tab.
  Future<void> notifyReceiveStarted({
    required String senderAlias,
    required List<String> fileNames,
    String? message,
  }) async {
    if (!isSupported) {
      return;
    }
    await ensureRegisteredOnce();
    final count = fileNames.length;
    final isFolder = fileNames.any((n) => n.contains('/'));
    final note = message?.trim();

    final String body;
    if (count == 0) {
      // R4: pure text message (text / clipboard text).
      body = t.notificationToasts.startMessage(preview: _oneLine(note ?? '', 60));
    } else if (note != null && note.isNotEmpty) {
      // R5: files + message note.
      final preview = _oneLine(note, 40);
      body = isFolder
          ? t.notificationToasts.startFolderNote(count: count, note: preview)
          : count == 1
              ? t.notificationToasts.startSingleFileNote(note: preview)
              : t.notificationToasts.startFilesNote(count: count, note: preview);
    } else if (isFolder) {
      // R3: folder.
      final top = _commonTopFolder(fileNames);
      body = top != null
          ? t.notificationToasts.startFolderNamed(folder: top, count: count)
          : t.notificationToasts.startFolder(count: count);
    } else if (count == 1) {
      // R1: single file.
      body = t.notificationToasts.startSingleFile(file: _clipFileName(fileNames.first));
    } else {
      // R2: multiple files.
      body = t.notificationToasts.startFiles(count: count);
    }

    await show(title: senderAlias, body: body, tag: 'receive-start', launch: 'action=home');
    _pendingOpenTarget = null;
    _pendingHome = true;
  }

  /// Toast for receiving finished (files/folder sessions only).
  /// Clicking behavior (D4):
  /// - single file  -> select that file in Explorer
  /// - folder       -> select the received folder root
  /// - multiple files -> open the receive directory
  Future<void> notifyReceiveFinished({
    required bool hasError,
    required int successCount,
    required int failedCount,
    required List<String> savedNames,
    required String? firstSavedPath,
    required String destinationDirectory,
  }) async {
    if (!isSupported) {
      return;
    }
    if (successCount == 0 && failedCount == 0) {
      return; // Nothing to announce (e.g. message-only sessions never call this).
    }
    await ensureRegisteredOnce();

    final isFolder = savedNames.any((n) => n.contains('/'));
    final String body;
    if (hasError) {
      // F3: partial/full failure.
      body = t.notificationToasts.finishPartial(count: successCount, failed: failedCount);
    } else if (successCount == 1) {
      // F1: single file.
      body = t.notificationToasts.finishSingleFile(
        file: _clipFileName(savedNames.isNotEmpty ? savedNames.first : ''),
      );
    } else if (isFolder) {
      // F2 folder.
      body = t.notificationToasts.finishFolder(count: successCount);
    } else {
      // F2 multiple files.
      body = t.notificationToasts.finishFiles(count: successCount);
    }

    ToastOpenTarget? target;
    if (firstSavedPath != null && firstSavedPath.isNotEmpty) {
      if (successCount == 1) {
        target = (folderPath: _dirOf(firstSavedPath), fileName: _baseName(firstSavedPath));
      } else if (isFolder) {
        final top = _commonTopFolder(savedNames);
        target = top != null
            ? (folderPath: destinationDirectory, fileName: top)
            : (folderPath: destinationDirectory, fileName: null);
      } else {
        target = (folderPath: destinationDirectory, fileName: null);
      }
    }

    await show(title: t.notificationToasts.appTitle, body: body, tag: 'receive-finish', launch: _revealLaunch(target));
    _pendingHome = target == null;
    _pendingOpenTarget = target;
  }

  /// Consumes the pending Explorer target (if any), clearing all pending
  /// state. Returns null when no Explorer open was requested.
  ToastOpenTarget? consumePendingOpenTarget() {
    final target = _pendingOpenTarget;
    _pendingOpenTarget = null;
    _pendingHome = false;
    return target;
  }

  /// Consumes the pending "go to receive home" action (only when no Explorer
  /// action is pending).
  bool consumePendingHomeAction() {
    if (_pendingOpenTarget != null) {
      return false;
    }
    if (!_pendingHome) {
      return false;
    }
    _pendingHome = false;
    return true;
  }

  /// Registers the native click callback (idempotent). Called lazily before the
  /// first toast is shown, so every displayed toast can be clicked.
  Future<void> attachActivationHandler() async {
    if (_activationAttached || !Platform.isWindows || _lib == null) {
      return;
    }
    try {
      final fn = _lib!.lookupFunction<
        ffi.Int32 Function(ffi.Pointer<ffi.NativeFunction<ffi.Void Function(ffi.Pointer<Utf16>)>>),
        int Function(ffi.Pointer<ffi.NativeFunction<ffi.Void Function(ffi.Pointer<Utf16>)>>)
      >('ls_toast_set_activation_callback');
      fn(_activationCallable.nativeFunction);
      _activationAttached = true;
      debugPrint('[WindowsNotification] activation handler attached');
    } catch (e) {
      debugPrint('[WindowsNotification] attachActivationHandler failed: $e');
    }
  }

  /// Native -> Dart entry point. Runs on a WinRT thread, so it must not block.
  static void _onNativeActivation(ffi.Pointer<Utf16> argsPtr) {
    final args = argsPtr.address == 0 ? '' : argsPtr.toDartString();
    _log('native activation: $args');
    debugPrint('[WindowsNotification] toast clicked: $args');
    // ignore: discarded_futures
    unawaited(_handleActivation(args));
  }

  /// Executes a toast click: brings the panel forward and, for finish toasts,
  /// reveals the received file/folder in Explorer. This does not depend on a
  /// second instance being launched, so it works whenever the app is running
  /// (including when it sits in the tray).
  static Future<void> _handleActivation(String args) async {
    _log('handle start args=$args');
    await _focusPanel();

    final Map<String, String> params;
    try {
      params = Uri.splitQueryString(args);
    } catch (e) {
      _log('bad launch payload: $e');
      return;
    }
    if (params['action'] != 'reveal') {
      _log('action=${params['action']} -> panel only');
      return;
    }
    final dir = params['dir'];
    if (dir == null || dir.isEmpty) {
      _log('reveal without dir');
      return;
    }
    final file = params['file'];
    try {
      await openFolder(folderPath: dir, fileName: (file == null || file.isEmpty) ? null : file);
      _log('revealed dir=$dir file=$file');
    } catch (e) {
      _log('reveal failed: $e');
    }
  }

  /// Shows and focuses the main window. Windows may refuse the first
  /// foreground request, so this retries and finally forces topmost briefly.
  static Future<void> _focusPanel() async {
    try {
      await showFromTray();
    } catch (e) {
      _log('showFromTray failed: $e');
    }
    try {
      await windowManager.restore();
      await windowManager.focus();
      for (var i = 0; i < 10; i++) {
        if (await windowManager.isFocused()) {
          _log('panel focused (attempt ${i + 1})');
          return;
        }
        await Future.delayed(const Duration(milliseconds: 120));
        await windowManager.focus();
      }
      // Last resort: a short topmost flip makes Windows grant the foreground.
      await windowManager.setAlwaysOnTop(true);
      await windowManager.focus();
      await windowManager.setAlwaysOnTop(false);
      _log('focus fallback used, focused=${await windowManager.isFocused()}');
    } catch (e) {
      _log('focus retry failed: $e');
    }
  }

  /// Diagnostic log for release builds (which have no console output).
  static void _log(String message) {
    try {
      final file = File('${Directory.systemTemp.path}${Platform.pathSeparator}localsend_toast_click.log');
      file.writeAsStringSync('${DateTime.now().toIso8601String()} $message\n', mode: FileMode.append, flush: true);
    } catch (_) {
      // Logging must never break activation handling.
    }
  }

  /// Builds the `launch` payload for a finish toast.
  static String _revealLaunch(ToastOpenTarget? target) {
    if (target == null) {
      return 'action=home';
    }
    return Uri(
      queryParameters: <String, String>{
        'action': 'reveal',
        'dir': target.folderPath,
        if (target.fileName != null && target.fileName!.isNotEmpty) 'file': target.fileName!,
      },
    ).query;
  }

  // --- text/target helpers -------------------------------------------------

  static String _clipFileName(String name, {int max = 50}) {
    if (name.length <= max) {
      return name;
    }
    return '${name.substring(0, 30)}…${name.substring(name.length - 15)}';
  }

  static String _oneLine(String text, int max) {
    final flat = text.replaceAll(RegExp(r'\s+'), ' ').trim();
    if (flat.length <= max) {
      return flat;
    }
    return '${flat.substring(0, max)}…';
  }

  /// First path segment shared by all folder file names, if any.
  static String? _commonTopFolder(List<String> fileNames) {
    String? first;
    for (final name in fileNames) {
      final slash = name.indexOf('/');
      final top = slash == -1 ? null : name.substring(0, slash);
      if (top == null || top.isEmpty) {
        return null; // Mixed top-level files -> not a clean single folder.
      }
      if (first == null) {
        first = top;
      } else if (first != top) {
        return null;
      }
    }
    return first;
  }

  static String _dirOf(String path) {
    final i = path.lastIndexOf(RegExp(r'[\\/]'));
    return i == -1 ? path : path.substring(0, i);
  }

  static String _baseName(String path) {
    final i = path.lastIndexOf(RegExp(r'[\\/]'));
    return i == -1 ? path : path.substring(i + 1);
  }

  /// Runs [action] on a background isolate with a fresh DLL handle, so the
  /// blocking native call and its COM initialization never touch the UI
  /// thread. Returns the native result code (0 = ok, null = skipped/error).
  Future<int?> _runInIsolate(
    int Function(ffi.DynamicLibrary lib) action,
  ) async {
    try {
      final code = await Isolate.run(() {
        final lib = ffi.DynamicLibrary.open(_dllName);
        return action(lib);
      });
      if (code != 0) {
        debugPrint('[WindowsNotification] native call failed (code $code)');
      }
      return code;
    } catch (e) {
      debugPrint('[WindowsNotification] show/ensure failed: $e');
      return null;
    }
  }

  /// Like [_withNativeString] but passes nullptr for null/empty values, which
  /// the native side treats as "not set".
  static int _withOptionalNativeString(
    String? value,
    int Function(ffi.Pointer<ffi.Uint16>) action,
  ) {
    if (value == null || value.isEmpty) {
      return action(ffi.nullptr);
    }
    return _withNativeString(value, action);
  }

  static int _withNativeString(
    String value,
    int Function(ffi.Pointer<ffi.Uint16>) action,
  ) {
    final units = value.codeUnits;
    final ptr = calloc<ffi.Uint16>(units.length + 1);
    try {
      for (var i = 0; i < units.length; i++) {
        ptr[i] = units[i];
      }
      ptr[units.length] = 0;
      return action(ptr);
    } finally {
      calloc.free(ptr);
    }
  }
}
