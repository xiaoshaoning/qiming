fn main()
{
    // Ensure the webview frontend dist dir exists with a placeholder index.html.
    // `tauri::generate_context!()` (used by the GUI binary) panics at compile
    // time when `build.frontendDist` does not exist, which breaks `cargo build`
    // / `cargo test` on a fresh clone before `npm run build` has been run.
    // The placeholder is overwritten by the real bundle once webview is built.
    let webview_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("webview");
    let dist_dir = webview_dir.join("dist");
    let index = dist_dir.join("index.html");
    if !index.exists() {
        std::fs::create_dir_all(&dist_dir)
            .expect("failed to create webview/dist");
        std::fs::write(&index, PLACEHOLDER_INDEX)
            .expect("failed to write placeholder webview/dist/index.html");
        println!("cargo:warning=webview frontend not built; wrote placeholder webview/dist/index.html");
    }

    tauri_build::build();

    // Build libqsim C library via CMake
    let libqsim_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("libqsim");

    let build_dir = libqsim_dir.join("build");

    // Run CMake configure. A stale CMakeCache.txt from a different source path
    // (e.g. one created inside WSL for the same checkout) makes cmake refuse to
    // reuse the cache; wipe the build dir and retry once in that case.
    let configure = |build_dir: &std::path::Path| -> std::process::ExitStatus {
        std::process::Command::new("cmake")
            .args([
                "-B",
                build_dir.to_str().unwrap(),
                "-S",
                libqsim_dir.to_str().unwrap(),
                "-DBUILD_TESTING=OFF",
            ])
            .status()
            .expect("failed to run cmake configure")
    };

    let mut status = configure(&build_dir);
    if !status.success() && build_dir.exists() {
        std::fs::remove_dir_all(&build_dir)
            .expect("failed to remove stale cmake build dir");
        status = configure(&build_dir);
    }
    assert!(status.success(), "cmake configure failed");

    // Match C library config to Cargo profile
    let config = if cfg!(debug_assertions) { "Debug" } else { "Release" };

    let status = std::process::Command::new("cmake")
        .args(["--build", build_dir.to_str().unwrap(), "--config", config])
        .status()
        .expect("failed to run cmake build");

    assert!(status.success(), "cmake build failed");

    // Link to the static library.
    // MSVC multi-config puts output in a config subdirectory; single-config
    // generators (Linux/Mac) put it directly in the build directory.
    let msvc_config_dir = build_dir.join(config);
    if msvc_config_dir.exists() {
        println!("cargo:rustc-link-search={}", msvc_config_dir.to_str().unwrap());
    } else {
        println!("cargo:rustc-link-search={}", build_dir.to_str().unwrap());
    }
    println!("cargo:rustc-link-lib=static=qsim");
}

/// Placeholder page embedded when the webview frontend has not been built yet.
const PLACEHOLDER_INDEX: &str = r#"<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Qiming Simulator</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #111; color: #eee;
           display: flex; min-height: 100vh; margin: 0; align-items: center; justify-content: center; }
    main { max-width: 34rem; text-align: center; padding: 2rem; }
    code { background: #222; padding: 0.1rem 0.4rem; border-radius: 4px; }
  </style>
</head>
<body>
  <main>
    <h1>Qiming Simulator</h1>
    <p>Frontend not built yet.</p>
    <p>Run <code>npm install && npm run build</code> in the
       <code>webview/</code> directory, then rebuild this app
       (<code>cargo build</code>) to embed the full GUI.</p>
  </main>
</body>
</html>
"#;
