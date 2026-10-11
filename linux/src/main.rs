use pegline::store::{Store, spectacle_folder, spectacle_recording_folder};
use serde_json::{Value, json};
use std::{
    cell::RefCell,
    ffi::{CStr, CString, c_char, c_int},
    fs,
    path::PathBuf,
};

thread_local! {
    static CONFIG: RefCell<Option<(Vec<PathBuf>, PathBuf)>> = const { RefCell::new(None) };
    static STORE: RefCell<Option<Store>> = const { RefCell::new(None) };
}

unsafe extern "C" {
    fn pg_run(argc: c_int, argv: *mut *mut c_char) -> c_int;
    fn pg_capture_valid(path: *const c_char) -> bool;
}

fn owned_json(value: Value) -> *mut c_char {
    CString::new(value.to_string())
        .expect("JSON escapes NUL bytes")
        .into_raw()
}

#[unsafe(no_mangle)]
extern "C" fn pg_snapshot() -> *mut c_char {
    STORE.with(|slot| {
        let mut slot = slot.borrow_mut();
        if slot.is_none() {
            let config = CONFIG.with(|config| config.borrow().clone()).expect("configured before Qt callbacks");
            match Store::open(config.0.clone(), config.1) {
                Ok(store) => *slot = Some(store),
                Err(e) => return owned_json(json!({"watch": config.0.first(), "recordings": config.0.last(), "items": [], "error": e.to_string()})),
            }
        }
        let store = slot.as_mut().unwrap();
        let _ = store.scan(|path| {
            CString::new(path.to_string_lossy().as_bytes()).is_ok_and(|path| unsafe { pg_capture_valid(path.as_ptr()) })
        });
        owned_json(json!({
            "watch": store.watches.first(),
            "recordings": store.watches.last(),
            "items": store.items.iter().map(|path| json!({"path": path.to_string_lossy(), "modified": store.modified(path).to_string(), "size": store.size(path).to_string(), "new": store.is_new_capture(path)})).collect::<Vec<_>>(),
            "error": store.error,
            "warning": store.warning,
        }))
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn pg_action(operation: *const c_char, path: *const c_char) -> *mut c_char {
    if operation.is_null() || path.is_null() {
        return owned_json(json!({"ok": false, "error": "Missing action argument"}));
    }
    let operation = unsafe { CStr::from_ptr(operation) }.to_string_lossy();
    let path = PathBuf::from(unsafe { CStr::from_ptr(path) }.to_string_lossy().as_ref());
    STORE.with(|slot| {
        let mut slot = slot.borrow_mut();
        let Some(store) = slot.as_mut() else {
            return owned_json(json!({"ok": false, "error": "Capture store is unavailable"}));
        };
        if !store.items.contains(&path) {
            return owned_json(json!({"ok": false, "error": "Capture is no longer on the line"}));
        }
        let result = match operation.as_ref() {
            "dismiss" => store.dismiss(&path).map(|_| json!({"ok": true})),
            "save" => store
                .save_copy(&path, &pictures_folder())
                .map(|target| json!({"ok": true, "saved": target.to_string_lossy()})),
            _ => Err(std::io::Error::other("Unknown store action")),
        };
        owned_json(result.unwrap_or_else(|e| json!({"ok": false, "error": e.to_string()})))
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn pg_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(unsafe { CString::from_raw(value) });
    }
}

// Called on a Qt worker only after Desktop validates membership on its UI thread.
#[unsafe(no_mangle)]
unsafe extern "C" fn pg_copy_video(path: *const c_char) -> *mut c_char {
    if path.is_null() {
        return owned_json(json!({"ok": false, "error": "Missing capture path"}));
    }
    let path = PathBuf::from(unsafe { CStr::from_ptr(path) }.to_string_lossy().as_ref());
    let result = pegline::store::copy_capture(&path, &videos_folder());
    owned_json(match result {
        Ok(target) => json!({"ok": true, "saved": target}),
        Err(error) => json!({"ok": false, "error": error.to_string()}),
    })
}

fn xdg(variable: &str, fallback: &str) -> PathBuf {
    std::env::var_os(variable)
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home().join(fallback))
}
fn home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

fn pictures_folder() -> PathBuf {
    media_folder("XDG_PICTURES_DIR=", "Pictures")
}

fn videos_folder() -> PathBuf {
    media_folder("XDG_VIDEOS_DIR=", "Videos")
}

fn media_folder(key: &str, fallback: &str) -> PathBuf {
    let settings = fs::read_to_string(xdg("XDG_CONFIG_HOME", ".config").join("user-dirs.dirs"))
        .unwrap_or_default();
    settings
        .lines()
        .find_map(|line| {
            line.strip_prefix(key).map(|value| {
                value
                    .trim_matches('"')
                    .replace("$HOME", &home().to_string_lossy())
            })
        })
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .unwrap_or_else(|| home().join(fallback))
}

fn main() {
    let arguments: Vec<_> = std::env::args().collect();
    let mut watch = None;
    let mut watch_videos = None;
    let mut i = 1;
    while i < arguments.len() {
        match arguments[i].as_str() {
            "--help" | "-h" => {
                println!(
                    "Pegline — captures, hanging within reach\n\nUsage: pegline [--watch DIRECTORY] [--watch-videos DIRECTORY] [--daemon|--capture-region|--show|--hide|--toggle|--status|--quit]\n\nWatches Spectacle screenshots and recordings by default.\n--watch replaces both defaults; --watch-videos adds a recordings folder.\nMeta+Shift+S: capture a region; new captures fly into the line for ten seconds.\nHover at the top center or press Meta+Alt+T to reveal.\nClick: copy • Double-click: open • Hold: annotate image / play video • Drag: share\nOriginal capture files are kept when cards are dismissed."
                );
                return;
            }
            "--version" | "-V" => {
                println!("Pegline {}", env!("CARGO_PKG_VERSION"));
                return;
            }
            "--watch" if i + 1 < arguments.len() => {
                i += 1;
                watch = Some(PathBuf::from(&arguments[i]));
            }
            "--watch-videos" if i + 1 < arguments.len() => {
                i += 1;
                watch_videos = Some(PathBuf::from(&arguments[i]));
            }
            "--daemon" | "--capture-region" | "--show" | "--hide" | "--toggle" | "--status"
            | "--quit" => {}
            value => {
                eprintln!("Unknown or incomplete option: {value}. Run pegline --help.");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let settings = fs::read_to_string(xdg("XDG_CONFIG_HOME", ".config").join("spectaclerc"))
        .unwrap_or_default();
    let custom_watch = watch.is_some();
    let mut watches = vec![watch.unwrap_or_else(|| spectacle_folder(&settings, &home()))];
    if let Some(videos) = watch_videos {
        watches.push(videos);
    } else if !custom_watch {
        watches.push(spectacle_recording_folder(
            &settings,
            &home(),
            &videos_folder(),
        ));
    }
    let state = xdg("XDG_STATE_HOME", ".local/state").join("pegline/state.json");
    CONFIG.with(|config| *config.borrow_mut() = Some((watches, state)));
    let strings: Vec<_> = arguments
        .iter()
        .map(|arg| CString::new(arg.as_bytes()).expect("argv cannot contain NUL"))
        .collect();
    let mut pointers: Vec<_> = strings.iter().map(|arg| arg.as_ptr().cast_mut()).collect();
    pointers.push(std::ptr::null_mut());
    let code = unsafe { pg_run(strings.len() as c_int, pointers.as_mut_ptr()) };
    std::process::exit(code);
}
