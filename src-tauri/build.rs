fn main()
{
    tauri_build::build();

    // Build libqsim C library via CMake
    let libqsim_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("libqsim");

    let build_dir = libqsim_dir.join("build");

    // Run CMake configure
    let status = std::process::Command::new("cmake")
        .args([
            "-B",
            build_dir.to_str().unwrap(),
            "-S",
            libqsim_dir.to_str().unwrap(),
            "-DBUILD_TESTING=OFF",
        ])
        .status()
        .expect("failed to run cmake configure");

    assert!(status.success(), "cmake configure failed");

    // Match C library config to Cargo profile
    let config = if cfg!(debug_assertions) { "Debug" } else { "Release" };

    let status = std::process::Command::new("cmake")
        .args(["--build", build_dir.to_str().unwrap(), "--config", config])
        .status()
        .expect("failed to run cmake build");

    assert!(status.success(), "cmake build failed");

    // Link to the static library (MSVC uses config subdirectory)
    println!("cargo:rustc-link-search={}/{}", build_dir.to_str().unwrap(), config);
    println!("cargo:rustc-link-lib=static=qsim");
}
