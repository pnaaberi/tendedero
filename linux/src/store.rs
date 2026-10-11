use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
    time::UNIX_EPOCH,
};

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
struct Fingerprint {
    modified: u64,
    size: u64,
}

#[derive(Default, Deserialize, Serialize)]
struct Saved {
    #[serde(default)]
    roots: BTreeMap<PathBuf, PathBuf>,
    items: Vec<PathBuf>,
    known: BTreeMap<PathBuf, Fingerprint>,
}

pub struct Store {
    pub watches: Vec<PathBuf>,
    pub items: Vec<PathBuf>,
    pub error: Option<String>,
    pub warning: Option<String>,
    state: PathBuf,
    roots: BTreeMap<PathBuf, PathBuf>,
    known: BTreeMap<PathBuf, Fingerprint>,
    samples: BTreeMap<PathBuf, Fingerprint>,
    startup: Option<BTreeMap<PathBuf, Fingerprint>>,
    rejected: BTreeMap<PathBuf, (Fingerprint, u8)>,
    dirty: bool,
}
impl Store {
    pub fn open(folders: Vec<PathBuf>, state: PathBuf) -> io::Result<Self> {
        let mut warning = None;
        let mut saved = match fs::read(&state) {
            Ok(bytes) => match serde_json::from_slice::<Saved>(&bytes) {
                Ok(saved) => saved,
                Err(e) => {
                    let stamp = std::time::SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_nanos();
                    let backup = state.with_file_name(format!("state.corrupt-{stamp}.json"));
                    fs::rename(&state, &backup)?;
                    warning = Some(format!(
                        "Invalid state preserved at {}: {e}",
                        backup.display()
                    ));
                    Saved::default()
                }
            },
            Err(e) if e.kind() == io::ErrorKind::NotFound => Saved::default(),
            Err(e) => return Err(e),
        };
        let mut watches = Vec::new();
        let mut roots = BTreeMap::new();
        for folder in folders {
            let folder = if folder.is_absolute() {
                folder
            } else {
                std::env::current_dir()?.join(folder)
            };
            // Keep each configured root's identity across a temporary outage.
            // Scan errors are independent, so one root cannot stall the other.
            let _ = fs::create_dir_all(&folder);
            let resolved = fs::canonicalize(&folder).unwrap_or_else(|_| {
                saved.roots.get(&folder).cloned().unwrap_or_else(|| {
                    fs::read_link(&folder)
                        .map(|target| {
                            if target.is_absolute() {
                                target
                            } else {
                                folder.parent().unwrap().join(target)
                            }
                        })
                        .unwrap_or_else(|_| folder.clone())
                })
            });
            roots.insert(folder, resolved.clone());
            if !watches.contains(&resolved) {
                watches.push(resolved);
            }
        }
        if watches.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "No capture folders",
            ));
        }
        let dirty = roots != saved.roots;
        saved.known.retain(|path, _| {
            watches
                .iter()
                .any(|folder| path.parent() == Some(folder.as_path()))
        });
        saved.items.retain(|path| saved.known.contains_key(path));
        saved.items.dedup();
        if saved.items.len() > 8 {
            saved.items.drain(..saved.items.len() - 8);
        }
        Ok(Self {
            watches,
            roots,
            items: saved.items,
            state,
            known: saved.known,
            samples: BTreeMap::new(),
            startup: None,
            rejected: BTreeMap::new(),
            error: None,
            warning,
            dirty,
        })
    }

    /// Decode only changed, stable files. A failed decode remains eligible for retry.
    pub fn scan(&mut self, valid: impl Fn(&Path) -> bool) -> io::Result<bool> {
        self.error = None;
        let result = self.scan_files(valid);
        if let Err(e) = &result {
            self.error = Some(format!("Cannot refresh captures: {e}"));
        }
        result
    }

    fn scan_files(&mut self, valid: impl Fn(&Path) -> bool) -> io::Result<bool> {
        let mut current = BTreeMap::new();
        let mut scan_error = None;
        for folder in &self.watches {
            match scan_directory(folder) {
                Ok(files) => current.extend(files),
                Err(error) => {
                    current.extend(
                        self.known
                            .iter()
                            .filter(|(path, _)| path.parent() == Some(folder.as_path()))
                            .map(|(path, stamp)| (path.clone(), *stamp)),
                    );
                    scan_error = Some(io::Error::new(
                        error.kind(),
                        format!("{}: {error}", folder.display()),
                    ));
                }
            }
        }
        self.startup.get_or_insert_with(|| current.clone());
        let before = self.items.clone();
        let before_known = self.known.clone();
        self.items.retain(|path| current.contains_key(path));
        self.known.retain(|path, _| current.contains_key(path));
        self.rejected
            .retain(|path, (stamp, _)| current.get(path) == Some(stamp));
        let mut arrivals: Vec<_> = current
            .iter()
            .filter(|(path, stamp)| {
                self.samples.get(*path) == Some(*stamp) && self.known.get(*path) != Some(*stamp)
            })
            .collect();
        arrivals.sort_by_key(|(path, stamp)| (stamp.modified, *path));
        for (path, stamp) in arrivals {
            if !valid(path) {
                let (_, attempts) = self.rejected.entry(path.clone()).or_insert((*stamp, 0));
                *attempts = attempts.saturating_add(1);
                if *attempts >= 3 && self.error.is_none() {
                    self.error = Some(format!("Cannot read capture: {}", path.display()));
                }
                continue;
            }
            self.rejected.remove(path);
            self.items.retain(|item| item != path);
            self.items.push(path.clone());
            self.known.insert(path.clone(), *stamp);
            if self.items.len() > 8 {
                self.items.remove(0);
            }
        }
        self.samples = current;
        let changed = self.items != before || self.known != before_known;
        self.dirty |= changed;
        if self.dirty {
            self.persist()?;
        }
        if let Some(error) = scan_error {
            return Err(error);
        }
        Ok(changed)
    }

    pub fn dismiss(&mut self, path: &Path) -> io::Result<()> {
        self.items.retain(|item| item != path);
        self.dirty = true;
        self.persist()
    }

    pub fn modified(&self, path: &Path) -> u64 {
        self.known.get(path).map_or(0, |stamp| stamp.modified)
    }

    pub fn size(&self, path: &Path) -> u64 {
        self.known.get(path).map_or(0, |stamp| stamp.size)
    }

    /// Historical files may finish settling after the UI's first snapshot.
    pub fn is_new_capture(&self, path: &Path) -> bool {
        self.known.get(path).is_some_and(|stamp| {
            self.startup
                .as_ref()
                .is_some_and(|initial| initial.get(path) != Some(stamp))
        })
    }

    pub fn save_copy(&self, path: &Path, destination: &Path) -> io::Result<PathBuf> {
        if !self.items.iter().any(|item| item == path) {
            return Err(io::Error::other("Capture is no longer on the line"));
        }
        copy_capture(path, destination)
    }

    fn persist(&mut self) -> io::Result<()> {
        use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
        let parent = self
            .state
            .parent()
            .ok_or_else(|| io::Error::other("Missing state directory"))?;
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(parent)?;
        let bytes = serde_json::to_vec(&Saved {
            roots: self.roots.clone(),
            items: self.items.clone(),
            known: self.known.clone(),
        })
        .map_err(io::Error::other)?;
        let temporary = self.state.with_extension("json.tmp");
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        fs::rename(temporary, &self.state)?;
        self.dirty = false;
        Ok(())
    }
}

/// Copy a capture without accessing store state; safe to run on a worker.
pub fn copy_capture(path: &Path, destination: &Path) -> io::Result<PathBuf> {
    fs::create_dir_all(destination)?;
    let name = path
        .file_name()
        .ok_or_else(|| io::Error::other("Missing filename"))?;
    let stem = path.file_stem().unwrap_or(name).to_string_lossy();
    let ext = path
        .extension()
        .map(|e| format!(".{}", e.to_string_lossy()))
        .unwrap_or_default();
    for n in 1..10000 {
        let target = destination.join(if n == 1 {
            name.to_os_string()
        } else {
            format!("{stem} ({n}){ext}").into()
        });
        let mut output = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&target)
        {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        };
        let copied = fs::File::open(path)
            .and_then(|mut input| io::copy(&mut input, &mut output))
            .and_then(|_| output.sync_all());
        if let Err(e) = copied {
            let _ = fs::remove_file(&target);
            return Err(e);
        }
        return Ok(target);
    }
    Err(io::Error::other("Too many filename collisions"))
}

fn scan_directory(folder: &Path) -> io::Result<BTreeMap<PathBuf, Fingerprint>> {
    let mut current = BTreeMap::new();
    for entry in fs::read_dir(folder)? {
        let path = entry?.path();
        if !candidate(&path) {
            continue;
        }
        let metadata = match fs::symlink_metadata(&path) {
            Ok(m) => m,
            Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
            Err(e) => return Err(e),
        };
        if !metadata.file_type().is_file() || metadata.len() == 0 {
            continue;
        }
        let modified = metadata
            .modified()?
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
            .min(u64::MAX as u128) as u64;
        current.insert(
            path,
            Fingerprint {
                modified,
                size: metadata.len(),
            },
        );
    }
    Ok(current)
}

fn candidate(path: &Path) -> bool {
    if path
        .file_name()
        .is_none_or(|name| name.to_string_lossy().starts_with('.'))
    {
        return false;
    }
    path.extension().is_some_and(|ext| {
        matches!(
            ext.to_string_lossy().to_lowercase().as_str(),
            "png" | "jpg" | "jpeg" | "webp" | "bmp" | "gif" | "tif" | "tiff" | "mp4" | "webm"
        )
    })
}

pub fn spectacle_folder(settings: &str, home: &Path) -> PathBuf {
    spectacle_save_folder(
        settings,
        home,
        "[ImageSave]",
        "imageSaveLocation=",
        home.join("Pictures/Screenshots"),
    )
}

pub fn spectacle_recording_folder(settings: &str, home: &Path, videos: &Path) -> PathBuf {
    let mut subfolder = "Screencasts";
    let mut in_videos = false;
    for line in settings.lines().map(str::trim) {
        if line.starts_with('[') {
            in_videos = line == "[VideoSave]";
        }
        if in_videos
            && let Some(value) = line.strip_prefix("translatedScreencastsFolder=")
            && !value.is_empty()
            && !value.contains('/')
            && value != "."
            && value != ".."
        {
            subfolder = value;
        }
    }
    spectacle_save_folder(
        settings,
        home,
        "[VideoSave]",
        "videoSaveLocation=",
        videos.join(subfolder),
    )
}

fn spectacle_save_folder(
    settings: &str,
    home: &Path,
    group: &str,
    key: &str,
    fallback: PathBuf,
) -> PathBuf {
    let mut in_group = false;
    for line in settings.lines().map(str::trim) {
        if line.starts_with('[') {
            in_group = line == group;
            continue;
        }
        if in_group
            && let Some(value) = line.strip_prefix(key)
            && !value.is_empty()
        {
            let value = value.strip_prefix("file://").unwrap_or(value);
            let bytes = value.as_bytes();
            let mut decoded = Vec::new();
            let mut i = 0;
            while i < bytes.len() {
                if bytes[i] == b'%'
                    && i + 2 < bytes.len()
                    && let Ok(hex) = std::str::from_utf8(&bytes[i + 1..i + 3])
                    && let Ok(n) = u8::from_str_radix(hex, 16)
                {
                    decoded.push(n);
                    i += 3;
                } else {
                    decoded.push(bytes[i]);
                    i += 1;
                }
            }
            let value = String::from_utf8_lossy(&decoded);
            let value = value.replace("$HOME", &home.to_string_lossy());
            let path = if let Some(rest) = value.strip_prefix("~/") {
                home.join(rest)
            } else {
                PathBuf::from(value)
            };
            if path.is_absolute() {
                return path;
            }
        }
    }
    fallback
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        sync::atomic::{AtomicUsize, Ordering},
    };

    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let path = std::env::temp_dir().join(format!(
                "pegline-test-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            fs::create_dir(path.join("shots")).unwrap();
            Self(path)
        }
        fn store(&self) -> Store {
            Store::open(vec![self.0.join("shots")], self.0.join("state.json")).unwrap()
        }
        fn image(&self, name: &str) -> PathBuf {
            let path = self.0.join("shots").join(name);
            fs::write(&path, b"fixture image contents").unwrap();
            path
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn waits_for_stability_and_retries_failed_decodes() {
        let f = Fixture::new();
        let mut s = f.store();
        let path = f.image("shot.png");
        s.scan(|_| true).unwrap();
        assert!(
            s.items.is_empty(),
            "first sample must not import a partial write"
        );
        s.scan(|_| false).unwrap();
        assert!(
            s.items.is_empty(),
            "failed image decode must not consume capacity"
        );
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, vec![path]);
    }

    #[test]
    fn separates_startup_imports_from_live_captures() {
        let f = Fixture::new();
        let historical = f.image("historical.png");
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.items.contains(&historical));
        assert!(
            !s.is_new_capture(&historical),
            "startup import must stay quiet"
        );

        let live = f.image("live.png");
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.is_new_capture(&live), "live capture must animate");
        fs::write(&historical, b"a changed screenshot after startup").unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.is_new_capture(&historical), "changed image must animate");

        let offline = f.image("offline.png");
        let mut restarted = f.store();
        restarted.scan(|_| true).unwrap();
        restarted.scan(|_| true).unwrap();
        assert!(restarted.items.contains(&offline));
        for path in [&historical, &live, &offline] {
            assert!(
                !restarted.is_new_capture(path),
                "restart must restore quietly"
            );
        }
    }

    #[test]
    fn filters_hidden_files_unsupported_formats_symlinks_and_empty_captures() {
        let f = Fixture::new();
        let image = f.image("actual.PNG");
        f.image("document.pdf");
        f.image(".hidden.png");
        fs::write(f.0.join("shots/empty.png"), b"").unwrap();
        std::os::unix::fs::symlink(&image, f.0.join("shots/link.png")).unwrap();
        fs::create_dir(f.0.join("shots/directory.png")).unwrap();
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, vec![image]);
    }

    #[test]
    fn combines_screenshots_and_recordings_with_shared_history_and_capacity() {
        let f = Fixture::new();
        let videos = f.0.join("videos");
        fs::create_dir(&videos).unwrap();
        let shot = f.image("shot.png");
        let clip = videos.join("clip.webm");
        let folders = vec![f.0.join("shots"), videos.clone()];
        let mut s = Store::open(folders.clone(), f.0.join("state.json")).unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        fs::write(&clip, b"synthetic recording").unwrap();
        s.scan(|_| true).unwrap();
        assert!(
            !s.items.contains(&clip),
            "recordings also need a stable second sample"
        );
        s.scan(|_| true).unwrap();
        assert!(s.items.contains(&shot) && s.items.contains(&clip));
        assert!(s.is_new_capture(&clip));
        let mut restarted = Store::open(folders, f.0.join("state.json")).unwrap();
        restarted.scan(|_| true).unwrap();
        restarted.scan(|_| true).unwrap();
        assert!(
            !restarted.is_new_capture(&clip),
            "restored recordings stay quiet"
        );
        restarted.dismiss(&clip).unwrap();
        restarted.scan(|_| true).unwrap();
        assert!(!restarted.items.contains(&clip) && clip.exists());
        for n in 0..10 {
            fs::write(videos.join(format!("clip-{n}.MP4")), b"synthetic recording").unwrap();
        }
        restarted.scan(|_| true).unwrap();
        restarted.scan(|_| true).unwrap();
        assert_eq!(restarted.items.len(), 8);
        assert!(
            shot.exists() && clip.exists(),
            "capacity never removes originals"
        );
    }

    #[test]
    fn unavailable_recording_folder_does_not_stall_screenshot_imports() {
        let f = Fixture::new();
        let videos = f.0.join("videos");
        let mut s = Store::open(
            vec![f.0.join("shots"), videos.clone()],
            f.0.join("state.json"),
        )
        .unwrap();
        let clip = videos.join("clip.webm");
        fs::write(&clip, b"synthetic recording").unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        fs::rename(&videos, f.0.join("offline-videos")).unwrap();
        let shot = f.image("new.png");
        assert!(s.scan(|_| true).is_err());
        assert!(s.scan(|_| true).is_err());
        assert!(s.items.contains(&shot) && s.items.contains(&clip));
        fs::rename(f.0.join("offline-videos"), &videos).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.error.is_none());
    }

    #[test]
    fn dismissal_survives_restart_and_changed_image_returns() {
        let f = Fixture::new();
        let path = f.image("shot.png");
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        s.dismiss(&path).unwrap();
        assert!(path.exists(), "dismissal must retain original");
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.items.is_empty());
        fs::write(&path, b"updated and larger image contents").unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, vec![path]);
    }

    #[test]
    fn capacity_eviction_does_not_delete_or_reimport_originals() {
        let f = Fixture::new();
        for n in 0..10 {
            f.image(&format!("shot-{n}.png"));
        }
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items.len(), 8);
        assert_eq!(fs::read_dir(f.0.join("shots")).unwrap().count(), 10);
        let before = s.items.clone();
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, before);
    }

    #[test]
    fn prunes_removed_files_but_scan_error_keeps_history() {
        let f = Fixture::new();
        let path = f.image("shot.png");
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        fs::rename(f.0.join("shots"), f.0.join("unavailable")).unwrap();
        assert!(s.scan(|_| true).is_err());
        assert_eq!(s.items.len(), 1);
        fs::rename(f.0.join("unavailable"), f.0.join("shots")).unwrap();
        fs::remove_file(path).unwrap();
        s.scan(|_| true).unwrap();
        assert!(s.items.is_empty());
    }

    #[test]
    fn copy_never_overwrites_existing_destination() {
        let f = Fixture::new();
        let source = f.image("shot.png");
        let pictures = f.0.join("pictures");
        fs::create_dir(&pictures).unwrap();
        fs::write(pictures.join("shot.png"), b"keep me").unwrap();
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        let dest = s.save_copy(&source, &pictures).unwrap();
        assert_eq!(dest.file_name().unwrap(), "shot (2).png");
        assert_eq!(fs::read(pictures.join("shot.png")).unwrap(), b"keep me");
        assert_eq!(fs::read(dest).unwrap(), fs::read(source).unwrap());
    }

    #[test]
    fn parses_spectacle_file_urls_and_paths_with_spaces() {
        let home = Path::new("/home/example");
        assert_eq!(
            spectacle_folder(
                "[ImageSave]\nimageSaveLocation=file:///home/example/My%20Shots\n",
                home
            ),
            PathBuf::from("/home/example/My Shots")
        );
        assert_eq!(
            spectacle_folder(
                "[ImageSave]\nimageSaveLocation=$HOME/Pictures/Screen shots\n",
                home
            ),
            PathBuf::from("/home/example/Pictures/Screen shots")
        );
        assert_eq!(
            spectacle_folder("[VideoSave]\nimageSaveLocation=/ignore/me\n", home),
            home.join("Pictures/Screenshots")
        );
    }

    #[test]
    fn recording_folder_uses_spectacle_setting_or_xdg_video_default() {
        let home = Path::new("/home/example");
        let videos = Path::new("/media/Videos");
        assert_eq!(
            spectacle_recording_folder("", home, videos),
            videos.join("Screencasts")
        );
        assert_eq!(
            spectacle_recording_folder(
                "[VideoSave]\nvideoSaveLocation=file:///media/My%20Clips\n[ImageSave]\nimageSaveLocation=/ignored\n",
                home,
                videos
            ),
            PathBuf::from("/media/My Clips")
        );
        assert_eq!(
            spectacle_recording_folder("[VideoSave]\nvideoSaveLocation=~/My clips\n", home, videos),
            home.join("My clips")
        );
        assert_eq!(
            spectacle_recording_folder(
                "[VideoSave]\ntranslatedScreencastsFolder=Tallenteet\n",
                home,
                videos
            ),
            videos.join("Tallenteet")
        );
        assert_eq!(
            spectacle_recording_folder(
                "[VideoSave]\ntranslatedScreencastsFolder=../elsewhere\nvideoSaveLocation=relative\n",
                home,
                videos
            ),
            videos.join("Screencasts")
        );
    }

    #[test]
    fn shared_folder_is_scanned_once_and_growing_video_waits_for_stability() {
        let f = Fixture::new();
        let folder = f.0.join("shots");
        let mut s = Store::open(vec![folder.clone(), folder], f.0.join("state.json")).unwrap();
        assert_eq!(s.watches.len(), 1);
        s.scan(|_| true).unwrap();
        let clip = f.image("recording.webm");
        s.scan(|_| panic!("first sample is not stable")).unwrap();
        fs::write(&clip, b"recording is still growing").unwrap();
        s.scan(|_| panic!("changed sample is not stable")).unwrap();
        assert!(s.items.is_empty());
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, vec![clip]);
    }

    #[test]
    fn unavailable_root_at_startup_does_not_disable_other_folder() {
        let f = Fixture::new();
        // A regular file in place of a directory fails even when tests run as root.
        let unavailable = f.0.join("offline");
        fs::write(&unavailable, b"not a directory").unwrap();
        let mut s = Store::open(
            vec![f.0.join("shots"), unavailable.join("videos")],
            f.0.join("state.json"),
        )
        .unwrap();
        let shot = f.image("shot.png");
        assert!(s.scan(|_| true).is_err());
        assert!(s.scan(|_| true).is_err());
        assert_eq!(s.items, vec![shot.clone()]);
        assert!(!s.is_new_capture(&shot));
    }

    #[test]
    fn symlinked_recording_root_retains_dismissals_across_offline_restart() {
        use std::os::unix::fs::symlink;
        let f = Fixture::new();
        let target = f.0.join("mounted-videos");
        let alias = f.0.join("videos");
        fs::create_dir(&target).unwrap();
        symlink(&target, &alias).unwrap();
        let clip = target.join("dismissed.mp4");
        fs::write(&clip, b"fixture recording").unwrap();
        let folders = vec![f.0.join("shots"), alias];
        let state = f.0.join("state.json");
        let mut s = Store::open(folders.clone(), state.clone()).unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        s.dismiss(&clip).unwrap();
        fs::rename(&target, f.0.join("offline")).unwrap();
        let mut s = Store::open(folders, state).unwrap();
        let shot = f.image("new-shot.png");
        assert!(s.scan(|_| true).is_err());
        assert!(s.scan(|_| true).is_err());
        assert_eq!(s.items, vec![shot.clone()]);
        fs::rename(f.0.join("offline"), &target).unwrap();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items, vec![shot]);
        assert!(!s.is_new_capture(&clip));
    }

    #[test]
    fn changing_watch_scope_with_unavailable_root_does_not_restore_unrelated_cards() {
        let f = Fixture::new();
        f.image("old-private-shot.png");
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        let blocked = f.0.join("unavailable");
        fs::write(&blocked, b"not a folder").unwrap();
        let mut s = Store::open(
            vec![f.0.join("new-shots"), blocked.join("videos")],
            f.0.join("state.json"),
        )
        .unwrap();
        assert!(s.items.is_empty());
        assert!(s.scan(|_| true).is_err());
        assert!(s.scan(|_| true).is_err());
        assert!(s.items.is_empty() && s.known.is_empty());
    }

    #[test]
    fn preserves_corrupt_state_for_recovery() {
        let f = Fixture::new();
        f.image("shot.png");
        fs::write(f.0.join("state.json"), b"{broken").unwrap();
        let mut s = f.store();
        s.scan(|_| true).unwrap();
        s.scan(|_| true).unwrap();
        assert_eq!(s.items.len(), 1);
        assert!(
            s.warning.is_some(),
            "state recovery notice must survive a successful scan"
        );
        assert!(fs::read_dir(&f.0).unwrap().any(|entry| {
            entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with("state.corrupt-")
        }));
    }

    #[test]
    fn settings_with_literal_percent_and_unicode_do_not_panic() {
        assert_eq!(
            spectacle_folder(
                "[ImageSave]\nimageSaveLocation=file:///home/example/%€ Screenshots\n",
                Path::new("/home/example")
            ),
            PathBuf::from("/home/example/%€ Screenshots")
        );
    }

    #[test]
    fn transient_scan_error_clears_when_folder_returns() {
        let f = Fixture::new();
        let mut s = f.store();
        fs::rename(f.0.join("shots"), f.0.join("unavailable")).unwrap();
        assert!(s.scan(|_| true).is_err());
        assert!(s.error.is_some(), "failed scan must report its error");
        fs::rename(f.0.join("unavailable"), f.0.join("shots")).unwrap();
        s.scan(|_| true).unwrap();
        assert!(
            s.error.is_none(),
            "recovered folder must not stay in error state"
        );
    }

    #[test]
    fn repeatedly_rejected_image_reports_error_and_can_recover() {
        let f = Fixture::new();
        let path = f.image("broken.png");
        let mut s = f.store();
        s.scan(|_| false).unwrap();
        s.scan(|_| false).unwrap();
        assert!(
            s.error.is_none(),
            "brief incomplete writes should not notify"
        );
        s.scan(|_| false).unwrap();
        s.scan(|_| false).unwrap();
        assert!(
            s.error
                .as_ref()
                .is_some_and(|error| error.contains("broken.png")),
            "rejected image needs an explanation"
        );
        s.scan(|_| true).unwrap();
        assert!(s.error.is_none());
        assert_eq!(s.items, vec![path]);
    }
}
