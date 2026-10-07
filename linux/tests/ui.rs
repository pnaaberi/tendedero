#[test]
fn native_ui_smoke() {
    let status = std::process::Command::new("bash")
        .arg("scripts/check-ui.sh")
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .status()
        .expect("run native Qt UI checks");
    assert!(status.success(), "native Qt interface checks failed");
}
