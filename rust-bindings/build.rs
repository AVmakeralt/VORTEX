use std::path::PathBuf;

fn main() {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let vortex_src = PathBuf::from(&manifest_dir).join("..").join("src");
    let vortex_build = PathBuf::from(&manifest_dir).join("..").join("build");

    let mut c_files = Vec::new();
    collect_c_files(&vortex_src, &mut c_files);

    let mut cc_build = cc::Build::new();
    cc_build
        .compiler("gcc")
        .flag("-std=gnu17")
        .flag("-O3")
        .flag("-fPIC")
        .flag("-DNDEBUG")
        .flag("-Wno-unused-variable")
        .flag("-Wno-unused-function")
        .flag("-Wno-unused-but-set-variable")
        .flag("-Wno-unused-parameter")
        .flag("-Wno-sign-compare")
        .flag("-Wno-discarded-qualifiers")
        .flag("-Wno-incompatible-pointer-types");

    cc_build.include(&vortex_src);
    cc_build.include(&vortex_build);

    /* NOTE: VORTEX_ENABLE_* and VORTEX_CACHE_MAX_SIZE / T1_THRESHOLD /
     * T2_THRESHOLD / COMPILE_THREADS are all #define'd in
     * build/vortex_config.h (generated from src/vortex_config.h.in by
     * CMake). We do NOT pass them via -D here — doing so causes
     * "macro redefined" warnings on every translation unit because
     * the command-line definition and the header definition conflict.
     *
     * The header is included via -I$BUILD, which is added above. */
    cc_build.define("NDEBUG", None);

    for f in &c_files {
        cc_build.file(f);
    }
    cc_build.compile("vortex");

    let wrapper = PathBuf::from(&manifest_dir).join("wrapper.h");
    let out_path = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    #[cfg(feature = "bindgen")]
    {
        let clang_lib = PathBuf::from(std::env::var("LIBCLANG_PATH")
            .unwrap_or_else(|_| "/usr/lib".to_string()));
        let bindings = bindgen::Builder::default()
            .header(wrapper.to_str().unwrap())
            .clang_arg(format!("-I{}", vortex_src.display()))
            .clang_arg(format!("-I{}", vortex_build.display()))
            .clang_arg(format!("-isystem{}/lib/clang/15.0.0/include", clang_lib.display()))
            .clang_arg("-isystem/usr/lib/gcc/x86_64-linux-gnu/14/include")
            .clang_arg("-isystem/usr/include")
            /* Do NOT pass -DVORTEX_ENABLE_* here — they're already defined
             * in build/vortex_config.h (included via -I above). Passing them
             * via -D causes "macro redefined" warnings on every TU. */
            .allowlist_type("vtx_.*")
            .allowlist_function("vtx_.*")
            .allowlist_var("VTX_.*")
            .allowlist_var("VORTEX_.*")
            .allowlist_var("VT_OP_.*")
            .allowlist_type("VT_OP_.*")
            .layout_tests(false)
            .generate_inline_functions(true)
            .generate()
            .expect("Failed to generate bindings (install libclang)");
        bindings.write_to_file(out_path.join("bindings.rs"))
            .expect("Failed to write bindings");
    }

    #[cfg(not(feature = "bindgen"))]
    {
        let pregen = PathBuf::from(&manifest_dir).join("src").join("bindings.rs");
        if pregen.exists() {
            std::fs::copy(&pregen, out_path.join("bindings.rs"))
                .expect("Failed to copy pre-generated bindings");
        } else {
            std::fs::write(out_path.join("bindings.rs"),
                "// No bindings available. Enable 'runtime-bindgen' feature.\n")
                .unwrap();
        }
    }

    for f in &c_files {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    println!("cargo:rerun-if-changed={}", wrapper.display());
}

fn collect_c_files(dir: &PathBuf, files: &mut Vec<PathBuf>) {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                collect_c_files(&path, files);
            } else if path.extension().and_then(|e| e.to_str()) == Some("c") {
                if path.file_name().and_then(|n| n.to_str()) != Some("main_new.c") {
                    files.push(path);
                }
            }
        }
    }
}
