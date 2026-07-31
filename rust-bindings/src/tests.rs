//! Tests for VORTEX Rust bindings.

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_value_smi() {
        let v = Value::smi(42);
        assert!(v.is_smi());
        assert!(!v.is_double());
        assert_eq!(v.as_smi(), 42);
    }

    #[test]
    fn test_value_smi_negative() {
        let v = Value::smi(-100);
        assert!(v.is_smi());
        assert_eq!(v.as_smi(), -100);
    }

    #[test]
    fn test_value_double() {
        let v = Value::double(3.14);
        assert!(v.is_double());
        assert!(!v.is_smi());
        assert!((v.as_double() - 3.14).abs() < 1e-10);
    }

    #[test]
    fn test_value_boolean() {
        let t = Value::boolean(true);
        let f = Value::boolean(false);
        assert!(t.is_bool());
        assert!(f.is_bool());
        assert!(t.as_bool());
        assert!(!f.as_bool());
    }

    #[test]
    fn test_value_null() {
        let n = Value::null();
        assert!(n.is_null());
        assert!(!n.is_undefined());
    }

    #[test]
    fn test_value_undefined() {
        let u = Value::undefined();
        assert!(u.is_undefined());
        assert!(!u.is_null());
    }

    #[test]
    fn test_value_debug_format() {
        assert_eq!(format!("{:?}", Value::smi(42)), "smi(42)");
        assert_eq!(format!("{:?}", Value::null()), "null");
        assert_eq!(format!("{:?}", Value::undefined()), "undefined");
    }

    #[test]
    fn test_arena_create_destroy() {
        let arena = Arena::new();
        drop(arena);
        // If we get here without crashing, the test passes.
    }

    #[test]
    fn test_vortex_runtime_create() {
        let vortex = Vortex::new();
        assert!(vortex.is_ok());
        let vortex = vortex.unwrap();
        drop(vortex);
    }

    #[test]
    fn test_profile_create_destroy() {
        let profile = Profile::new();
        assert!(profile.is_ok());
        let profile = profile.unwrap();
        assert_eq!(profile.method_count(), 0);
        drop(profile);
    }

    #[test]
    fn test_profile_record_branch() {
        let mut profile = Profile::new().unwrap();
        profile.record_branch(1, 10, true);
        profile.record_branch(1, 10, true);
        profile.record_branch(1, 10, false);
        assert!(profile.method_count() > 0);
    }

    #[test]
    fn test_profile_record_invocation() {
        let mut profile = Profile::new().unwrap();
        for _ in 0..100 {
            profile.record_invocation(1);
        }
    }

    #[test]
    fn test_profile_save_load_roundtrip() {
        let dir = std::env::temp_dir();
        let path = dir.join("vortex_rust_test_profile.prof");

        // Create and populate a profile
        let mut profile = Profile::new().unwrap();
        profile.record_invocation(1);
        profile.record_branch(1, 10, true);
        profile.record_callsite_type(1, 0, 42);

        // Save
        let hash = [0xAAu8; 32];
        profile.save(&path, &hash).unwrap();

        // Load
        let loaded = Profile::load(&path, &hash);
        assert!(loaded.is_ok());
        let loaded = loaded.unwrap();
        assert!(loaded.method_count() > 0);

        // Clean up
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_confidence_monomorphic() {
        // A monomorphic callsite should have high confidence (1.0)
        // after the P10 fix.
        let mut cs = vtx_callsite_profile_t::default();
        cs.count = 1;
        cs.types[0] = 42;
        let conf = confidence::type_dist(&cs);
        assert_eq!(conf, 1.0);
    }

    #[test]
    fn test_confidence_branch_at_threshold() {
        let mut b = vtx_branch_profile_t::default();
        b.taken = VTX_CONFIDENCE_THRESHOLD_BRANCH as u64;
        let conf = confidence::branch(&b);
        assert_eq!(conf, 1.0);
    }

    #[test]
    fn test_confidence_eligible_for_tier() {
        // A method with high-confidence branches should be eligible for T2
        let mut b = vtx_branch_profile_t::default();
        b.taken = VTX_CONFIDENCE_THRESHOLD_BRANCH * 2;

        let mut method = vtx_profile_method_t::default();
        method.method_id = 1;
        method.invocation_count = VORTEX_T1_THRESHOLD as u64 * 10;  // hot
        method.branch_count = 1;
        method.branches = &mut b;
        method.branch_capacity = 1;

        assert!(confidence::eligible_for_tier(&method, VORTEX_T1_THRESHOLD as u64, 2));
    }

    #[test]
    fn test_deterministic_disabled_by_default() {
        deterministic::init();
        // In test environment, VORTEX_DETERMINISTIC is not set
        assert!(!deterministic::enabled());
    }

    #[test]
    fn test_ensemble_create_destroy() {
        let ensemble = Ensemble::new();
        assert!(ensemble.is_ok());
        let ensemble = ensemble.unwrap();
        drop(ensemble);
    }

    #[test]
    fn test_ensemble_outlier_resistance() {
        let mut ensemble = Ensemble::new().unwrap();

        // Add 4 good runs with P(taken) = 80%
        for _ in 0..4 {
            let mut profile = Profile::new().unwrap();
            for _ in 0..800 {
                profile.record_branch(1, 10, true);
            }
            for _ in 0..200 {
                profile.record_branch(1, 10, false);
            }
            let meta = RunMeta {
                sample_count: 10000,
                deopt_count: 0,
                deopt_rate: 0.0,
                runtime_duration_s: 10.0,
            };
            ensemble.add_run(&profile, meta).unwrap();
        }

        // Add 1 outlier run with P(taken) = 20%
        {
            let mut profile = Profile::new().unwrap();
            for _ in 0..200 {
                profile.record_branch(1, 10, true);
            }
            for _ in 0..800 {
                profile.record_branch(1, 10, false);
            }
            let meta = RunMeta {
                sample_count: 10000,
                deopt_count: 0,
                deopt_rate: 0.0,
                runtime_duration_s: 10.0,
            };
            ensemble.add_run(&profile, meta).unwrap();
        }

        // Compute aggregate
        let agg = ensemble.compute_aggregate();
        assert!(agg.is_some());

        // The median of [0.8, 0.8, 0.8, 0.8, 0.2] is 0.8, so the aggregate
        // should have P(taken) ≈ 80%, NOT 68% (simple average).
        let agg_ptr = agg.unwrap();
        unsafe {
            let method = vtx_profile_get_method(agg_ptr.as_ref(), 1);
            assert!(method.is_some());
            if let Some(m) = method {
                let b = &(*(*m)).branches[0];
                let total = b.taken + b.not_taken;
                let p_taken = b.taken as f64 / total as f64;
                // Should be close to 0.8 (median), not 0.68 (average)
                assert!(p_taken > 0.7, "P(taken) = {}, expected > 0.7 (median)", p_taken);
            }
        }
    }

    #[test]
    fn test_input_shape_size_bins() {
        assert_eq!(SizeBin::from_count(0), SizeBin::Empty);
        assert_eq!(SizeBin::from_count(1), SizeBin::One);
        assert_eq!(SizeBin::from_count(3), SizeBin::Tiny);
        assert_eq!(SizeBin::from_count(10), SizeBin::Small);
        assert_eq!(SizeBin::from_count(100), SizeBin::Medium);
        assert_eq!(SizeBin::from_count(1000), SizeBin::Large);
    }

    #[test]
    fn test_input_shape_different_sizes() {
        let small = InputShape::new(0x11, 0x22, SizeBin::Tiny, SizeBin::Tiny, 1, 0x44);
        let large = InputShape::new(0x11, 0x22, SizeBin::Large, SizeBin::Large, 1, 0x44);
        assert_ne!(small, large);
    }

    #[test]
    fn test_input_shape_same_inputs() {
        let s1 = InputShape::new(0x11, 0x22, SizeBin::Medium, SizeBin::Small, 3, 0x44);
        let s2 = InputShape::new(0x11, 0x22, SizeBin::Medium, SizeBin::Small, 3, 0x44);
        assert_eq!(s1, s2);
    }

    #[test]
    fn test_input_shape_table_create() {
        let table = InputShapeTable::new(42);
        assert!(table.is_ok());
        let table = table.unwrap();
        assert_eq!(table.shape_count(), 1);  // default shape
        drop(table);
    }

    #[test]
    fn test_input_shape_table_different_shapes_separate() {
        let mut table = InputShapeTable::new(1).unwrap();

        let shape_small = InputShape::new(0x11, 0x22, SizeBin::Tiny, SizeBin::Tiny, 1, 0x44);
        let shape_large = InputShape::new(0x11, 0x22, SizeBin::Large, SizeBin::Large, 1, 0x44);

        let m_small = table.get_or_create(shape_small);
        assert!(m_small.is_some());

        let m_large = table.get_or_create(shape_large);
        assert!(m_large.is_some());

        // Should now have 3 shapes: default + small + large
        assert_eq!(table.shape_count(), 3);

        // The two methods should be different pointers
        assert_ne!(m_small.unwrap(), m_large.unwrap());
    }

    #[test]
    fn test_input_shape_manager_create() {
        let mgr = InputShapeManager::new();
        assert!(mgr.is_ok());
        let mgr = mgr.unwrap();
        drop(mgr);
    }

    #[test]
    fn test_shape_dispatch_create() {
        let dispatch = ShapeDispatch::new();
        assert!(dispatch.is_ok());
        let dispatch = dispatch.unwrap();
        drop(dispatch);
    }

    #[test]
    fn test_shape_dispatch_install_lookup() {
        let mut dispatch = ShapeDispatch::new().unwrap();

        // Install default version
        let fake_default = 0x1000usize as *mut c_void;
        dispatch.install_default(1, fake_default).unwrap();

        // Install shape-specific version
        let shape = InputShape::new(0x11, 0x22, SizeBin::Large, SizeBin::Large, 1, 0x44);
        let fake_shape = 0x2000usize as *mut c_void;
        dispatch.install(1, shape, fake_shape).unwrap();

        // Look up the specific shape
        let result = dispatch.lookup(1, shape);
        assert_eq!(result, Some(fake_shape));

        // Look up an unknown shape — should fall back to default
        let unknown = InputShape::new(0x99, 0x88, SizeBin::Medium, SizeBin::Medium, 1, 0x77);
        let result2 = dispatch.lookup(1, unknown);
        assert_eq!(result2, Some(fake_default));
    }

    #[test]
    fn test_phase_partition_create() {
        let partition = PhasePartition::new();
        assert!(partition.is_ok());
        let partition = partition.unwrap();
        assert_eq!(partition.phase_count(), 1);  // default phase
        drop(partition);
    }

    #[test]
    fn test_phase_partition_transition() {
        let mut partition = PhasePartition::new().unwrap();
        assert_eq!(partition.active_phase(), ffi::VTX_PHASE_NONE);

        partition.transition(1);
        assert_eq!(partition.active_phase(), 1);

        partition.transition(2);
        assert_eq!(partition.active_phase(), 2);

        assert_eq!(partition.phase_count(), 3);  // default + 2 phases
    }

    #[test]
    fn test_t1_cache_filename() {
        let mut buf = [0u8; 600];
        let c_dir = CString::new("/tmp/profs").unwrap();
        let c_hash = CString::new("abcdef0123456789").unwrap();
        let rc = unsafe {
            vtx_t1_cache_filename(c_dir.as_ptr(), c_hash.as_ptr(), buf.as_mut_ptr() as *mut c_char, buf.len())
        };
        assert_eq!(rc, 0);
        let s = unsafe { CStr::from_ptr(buf.as_ptr() as *const c_char) };
        assert_eq!(s.to_str().unwrap(), "/tmp/profs/abcdef0123456789.t1c");
    }

    #[test]
    fn test_patch_log_open_close() {
        let path = std::env::temp_dir().join("vortex_rust_test_patchlog.vpl");
        let _ = std::fs::remove_file(&path);

        let hash = [0xBBu8; 32];
        let log = PatchLog::open(&path, &hash);
        assert!(log.is_ok());
        let log = log.unwrap();
        drop(log);  // closes the log

        // File should exist
        assert!(path.exists());

        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_patch_log_append_replay() {
        let path = std::env::temp_dir().join("vortex_rust_test_patchlog_replay.vpl");
        let _ = std::fs::remove_file(&path);

        let hash = [0xCCu8; 32];

        // Write
        {
            let mut log = PatchLog::open(&path, &hash).unwrap();
            log.append_branch(1, 10, 3, 1).unwrap();
            log.append_invocation(1, 100).unwrap();
        }

        // Replay
        {
            let mut log = PatchLog::open_read(&path, &hash).unwrap();
            let mut profile = Profile::new().unwrap();
            let applied = log.replay(&mut profile);
            assert!(applied.is_ok());
            assert!(applied.unwrap() >= 2);
        }

        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_patch_log_crash_recovery() {
        let path = std::env::temp_dir().join("vortex_rust_test_patchlog_crash.vpl");
        let _ = std::fs::remove_file(&path);

        let hash = [0xDDu8; 32];

        // Write 3 entries
        {
            let mut log = PatchLog::open(&path, &hash).unwrap();
            log.append_branch(1, 10, 10, 5).unwrap();
            log.append_branch(1, 20, 20, 10).unwrap();
            log.append_branch(1, 30, 30, 15).unwrap();
        }

        // Truncate the file to simulate a crash
        let file_size = std::fs::metadata(&path).unwrap().len();
        let truncated = file_size * 80 / 100;
        let f = std::fs::OpenOptions::new().write(true).open(&path).unwrap();
        f.set_len(truncated).unwrap();
        drop(f);

        // Replay — should recover at least some entries
        {
            let mut log = PatchLog::open_read(&path, &hash).unwrap();
            let mut profile = Profile::new().unwrap();
            let applied = log.replay(&mut profile);
            assert!(applied.is_ok());
            assert!(applied.unwrap() >= 1, "should recover at least 1 entry after crash");
        }

        let _ = std::fs::remove_file(&path);
    }
}
