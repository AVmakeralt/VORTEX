// build.rs — Compile the VORTEX C library and generate Rust FFI bindings.
//
// This script:
//   1. Compiles all C source files from ../src/ into a static library
//   2. Runs bindgen on wrapper.h to generate Rust FFI type/function declarations
//   3. Links the static library

use std::path::PathBuf;

fn main() {
    // ---- Find the VORTEX source directory ----
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let vortex_src = PathBuf::from(&manifest_dir).join("..").join("src");
    let vortex_build = PathBuf::from(&manifest_dir).join("..").join("build");

    // ---- Collect all C source files ----
    let mut c_files = Vec::new();
    collect_c_files(&vortex_src, &mut c_files);

    // ---- Determine compile flags (matching the CMake build) ----
    let mut cc_build = cc::Build::new();
    cc_build
        .compiler("gcc")
        .flag("-std=gnu17")
        .flag("-O3")
        .flag("-fPIC")
        .flag("-DNDEBUG")
        // Warnings (suppressed to match the C build)
        .flag("-Wno-unused-variable")
        .flag("-Wno-unused-function")
        .flag("-Wno-unused-but-set-variable")
        .flag("-Wno-unused-parameter")
        .flag("-Wno-sign-compare")
        .flag("-Wno-discarded-qualifiers")
        .flag("-Wno-incompatible-pointer-types");

    // Include paths
    cc_build.include(&vortex_src);
    cc_build.include(&vortex_build);

    // Feature flags
    cc_build.define("VORTEX_ENABLE_SOTA", None);
    cc_build.define("VORTEX_ENABLE_ASSERTIONS", None);
    cc_build.define("VORTEX_ENABLE_VERIFY", None);
    cc_build.define("VORTEX_ENABLE_PROFILING", None);

    // Config values (matching CMakeCache.txt defaults)
    cc_build.define("VORTEX_CACHE_MAX_SIZE", Some("268435456"));
    cc_build.define("VORTEX_T1_THRESHOLD", Some("1000"));
    cc_build.define("VORTEX_T2_THRESHOLD", Some("10000"));
    cc_build.define("VORTEX_COMPILE_THREADS", Some("0"));

    // Add all source files
    for f in &c_files {
        cc_build.file(f);
    }

    // Compile and link
    cc_build.compile("vortex");

    // ---- Generate FFI bindings with bindgen (if feature enabled) ----
    let wrapper = PathBuf::from(&manifest_dir).join("wrapper.h");

    let out_path = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    #[cfg(feature = "bindgen")]
    {
        let bindings = bindgen::Builder::default()
            .header(wrapper.to_str().unwrap())
            .clang_arg(format!("-I{}", vortex_src.display()))
            .clang_arg(format!("-I{}", vortex_build.display()))
            .clang_arg("-isystem/usr/lib/gcc/x86_64-linux-gnu/14/include")
            .clang_arg("-isystem/usr/include")
            .clang_arg("-DVORTEX_ENABLE_SOTA")
            .clang_arg("-DVORTEX_ENABLE_ASSERTIONS")
            .clang_arg("-DVORTEX_ENABLE_VERIFY")
            .clang_arg("-DVORTEX_ENABLE_PROFILING")
            .clang_arg("-DVORTEX_CACHE_MAX_SIZE=268435456")
            .clang_arg("-DVORTEX_T1_THRESHOLD=1000")
            .clang_arg("-DVORTEX_T2_THRESHOLD=10000")
            .clang_arg("-DVORTEX_COMPILE_THREADS=0")
            .allowlist_type("vtx_.*")
            .allowlist_function("vtx_.*")
            .allowlist_var("VTX_.*")
            .allowlist_var("VORTEX_.*")
            .allowlist_var("VT_OP_.*")
            .allowlist_var("VT_IC_.*")
            .allowlist_var("VTX_GC_.*")
            .allowlist_var("VTX_PHASE_.*")
            .allowlist_var("VTX_CONFIDENCE_.*")
            .allowlist_var("VTX_PROMOTION_.*")
            .allowlist_var("VTX_RECOMP_.*")
            .allowlist_var("VTX_ENSEMBLE_.*")
            .allowlist_var("VTX_INPUT_SHAPE_.*")
            .allowlist_var("VTX_SIZE_BIN_.*")
            .allowlist_var("VTX_T1_CACHE_.*")
            .allowlist_var("VTX_PATCH_.*")
            .allowlist_var("VTX_TAG_.*")
            .allowlist_var("VTX_NAN_.*")
            .allowlist_var("VTX_SMI_.*")
            .allowlist_var("VTX_POLY_.*")
            .allowlist_var("VTX_PROFILE_.*")
            .allowlist_var("VTX_MAX_.*")
            .allowlist_var("VTX_INLINE_.*")
            .allowlist_var("VTX_GUARD_.*")
            .allowlist_var("VTX_VALUE_.*")
            .allowlist_var("VTX_FEEDBACK_.*")
            .allowlist_var("VTX_LOOP_.*")
            .allowlist_var("VTX_SIDE_.*")
            .allowlist_var("VTX_STITCH_.*")
            .allowlist_var("VTX_TIER_.*")
            .allowlist_var("VTX_ORCHESTRATOR_.*")
            .allowlist_var("VTX_PHASE_PARTITION_.*")
            .allowlist_var("VTX_SHAPE_DISPATCH_.*")
            .allowlist_var("VTX_TRIP_.*")
            .allowlist_var("VTX_TYPE_FEEDBACK_.*")
            .allowlist_var("VTX_TYPE_STABILITY_.*")
            .allowlist_var("VTX_SHAPE_STABILITY_.*")
            .allowlist_var("VTX_TYPE_GUARD_.*")
            .allowlist_var("VTX_CACHE_.*")
            .allowlist_var("VTX_ARENA_.*")
            .allowlist_var("VTX_FRAME_.*")
            .allowlist_var("VTX_INTERP_.*")
            .allowlist_type("VT_OP_.*")
            .allowlist_type("VT_IC_.*")
            .allowlist_type("VTX_GC_.*")
            .allowlist_type("VTX_TIER_.*")
            .layout_tests(false)
            .generate_comments(true)
            .generate_inline_functions(true)
            .derive_debug(true)
            .derive_copy(true)
            .derive_default(true)
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
            .generate()
            .expect("Failed to generate bindings (ensure libclang is installed)");

        bindings
            .write_to_file(out_path.join("bindings.rs"))
            .expect("Failed to write bindings");
    }

    #[cfg(not(feature = "bindgen"))]
    {
        // Use pre-generated bindings
        let pregen = PathBuf::from(&manifest_dir).join("src").join("bindings.rs");
        if pregen.exists() {
            std::fs::copy(&pregen, out_path.join("bindings.rs"))
                .expect("Failed to copy pre-generated bindings");
        } else {
            // Generate minimal bindings stub — the C code still compiles,
            // but Rust users won't have FFI types. This allows the crate
            // to be published without libclang.
            std::fs::write(out_path.join("bindings.rs"),
                "// Pre-generated bindings not available. Enable 'runtime-bindgen' feature.\n")
                .expect("Failed to write bindings stub");
        }
    }

    // Tell cargo to invalidate when C files change
    for f in &c_files {
        println!("cargo:rerun-if-changed={}", f.display());
    }
    println!("cargo:rerun-if-changed={}", wrapper.display());
}

/// Recursively collect all .c files from a directory.
fn collect_c_files(dir: &PathBuf, files: &mut Vec<PathBuf>) {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                collect_c_files(&path, files);
            } else if path.extension().and_then(|e| e.to_str()) == Some("c") {
                // Skip main_new.c — it has a main() function
                if path.file_name().and_then(|n| n.to_str()) != Some("main_new.c") {
                    files.push(path);
                }
            }
        }
    }
}
