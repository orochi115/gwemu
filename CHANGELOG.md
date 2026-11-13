2026-11-13  gnw: close functional device gaps from the legacy comparison
            Enforce NOR write enable, one-way page programming and page wrap;
            model DAC channel enable/software trigger/dual holding aliases;
            apply PA4/DAC1 brightness to scanout; decode all eight LTDC
            formats on both layers; clear PWR status flags with self-clearing
            commands; restrict CNT/ARR to each timer's hardware width.

2026-10-15  ospi: select supported NOR chips up to 256 MiB
            The Flash tab and profile wizard now select a chip from the
            JEDEC catalog used by retro-go-sd. The selected capacity sets
            the extflash mapping and OSPI density; smaller source blobs
            are extended with erased-state 0xFF bytes and blobs that do
            not fit are rejected. Internal flash images also receive
            explicit 0xFF padding through the selected 256 KiB bank size.

2026-10-06  gpio: use one physical button/power-good pinout for all firmware
            The working game-and-watch-retro-go-sd board definitions put
            GAME on PC1, PAUSE on PC13, TIME on PC5, and charger PGOOD on
            PA2. The previous August stock-firmware interpretation swapped
            TIME/PGOOD. In Retro-Go it made TIME pull PA2 low and visibly
            report a power connection. Use the board pinout globally: TIME
            drives PC5; PA2 is held low as the emulator's powered PGOOD
            state. No firmware/profile switch chooses GPIO wiring.

2026-08-19  gpio: TIME is PA2, not PA2+PC5 -- pressing it no longer
            asserts the charger's PGOOD line. Stock Zelda supports two
            board variants, choosing the pin pair at runtime from a .bss
            byte at 0x2000ab92: the button matrix (0x08016808) reads TIME
            from PC5 when it is 1 and PA2 otherwise, and the active-low
            PGOOD sense (0x0800f004) takes the opposite pin of the pair.
            Nothing writes that byte, so this board is permanently the
            "TIME on PA2, PGOOD on PC5" variant. The old table drove both
            pins, which worked for TIME by accident while telling firmware
            external power had just been connected on every press. PC5 is
            now instead held low from reset, so the emulated unit always
            reads as plugged in (deliberate policy -- no battery or
            charger UI is modelled).

2026-08-14  spi: model SPI1's DMA request path, so retro-go-sd's
            DMA-based SD block reads actually complete. Firmware commit
            5e19e392e made SPI_RxBuffer() use
            HAL_SPI_TransmitReceive_DMA() for every 512-byte SD data
            block; with no DMA modelling at all each block instead ate
            the firmware's full 100ms HAL_SPI_GetState() timeout plus a
            HAL_SPI_Abort() before falling back to the polled path
            (~0.5 sector reads/sec, nothing booted inside a timeline).
            SPI1 now registers DMAMUX requests 37/38 with the DMA
            controller, does the real SSI byte exchange out of the RX
            stream's notifier (sourcing outgoing bytes from the TX
            stream's buffer), and raises EOT on NVIC 35 so
            HAL_SPI_IRQHandler retires the transfer. Two register-level
            bugs found live along the way and fixed here: the
            SVD-derived CR1 write mask (0xfd01) dropped CSTART (bit 9)
            entirely, so every transfer start was silently discarded;
            and the polled TXDR path never cleared CSTART, which left it
            stuck at 1 and made the next DMA transfer's streams look
            already-started before the guest armed them. Measured after:
            sustained 100-500 SD blocks/sec, guest at a full 59.6fps.
            GNW_SPI_DMA_TRACE=1 prints per-second block throughput.

2026-08-07  ospi: model the OCTOSPI's shared-pin constraint -- the
            memory-mapped XIP window at 0x9xxxxxxx now raises a bus
            fault whenever the controller is not in memory-mapped mode
            (CR.EN clear or CR.FMODE != 11), per RM0455 24.4.17's AXI
            slave error conditions. Previously extflash was a plain
            always-readable RAM region, so firmware reading or
            executing from external flash between HAL_OSPI_Abort() and
            the restore of FMODE=11 -- which faults on silicon -- ran
            fine here, making that whole defect class invisible to
            gwemu testing. The gate sits above OTFDEC's decrypt
            overlays and is up from reset; host-side accesses
            (-device loader, pmemsave, gdbstub) bypass it. CR.ABORT is
            now self-clearing per 24.4.18. Stock Zelda and retro-go
            both verified unaffected.

2026-07-30  ui: fix the Settings window never opening when Vulkan is
            unavailable. SDL3's SDL_HINT_RENDER_DRIVER is a whitelist,
            not a preference -- with it set, SDL_CreateRenderer() tries
            only the named drivers and fails outright otherwise. The
            Settings window was created while the Linux-only vulkan-only
            hint was in force, between the main renderer's first attempt
            and its fallback chain, with a single un-retried call: on any
            host where Vulkan can't initialize, its renderer came back
            NULL, display_early_init() skipped
            gwemu_settings_hud_init(), and every Settings menu item
            silently did nothing (the menu bar itself lives on the main
            window's context, so the app looked fine otherwise). Broken
            since the dedicated Settings window landed on 2026-07-24;
            found on the packaged 0.0.19 Linux build on a Radeon 860M
            host (VK_ERROR_INITIALIZATION_FAILED, GL unavailable too, so
            the main window fell back to software). The window is now
            created after the main renderer resolves and pinned to
            whichever driver actually worked for it, with the same
            reset-hint/software retreat as a backstop, and a failure
            reports itself (stderr + message box) instead of leaving dead
            menu items. New GNW_RENDER_DRIVER env var overrides the
            driver preference, since the only way to exercise the
            non-Vulkan paths on a Vulkan-capable host was to edit the
            source and rebuild.

2026-07-30  rtc: make SSR a real sub-second counter instead of a constant.
            RTC_SSR was a read-only shadow of its 0 reset value that
            nothing ever wrote, and with PRER's reset PREDIV_S of 255
            that pins the HAL's (PREDIV_S - SSR)/(PREDIV_S + 1) fraction
            at a constant 255/256 -- so GW_GetCurrentMillis() and
            _gettimeofday() reported tv_usec == 996000 forever and every
            sub-second delta computed from them came out zero. Reported
            from a retro-go session where gettimeofday()-based timing
            worked on real hardware and silently measured nothing here.
            SSR now down-counts from the programmed PREDIV_S across each
            calendar second off the same virtual-clock-anchored calendar
            TR/DR already used, and SSR/TR/DR reads take the hardware's
            shadow-register lock (latched on the first SSR-or-TR read,
            released on the closing DR read) so the triple the HAL reads
            in that order is always one coherent instant rather than
            three independent samples that can straddle a second
            boundary. The calendar anchor is also in vmstate now (v2); it
            was missing entirely, so a restored snapshot kept the
            destination's own reset-seeded base. Verified live against
            stock mario over gdb: SSR down-counts ~104/256 per 0.4s and
            rolls over exactly on the TR seconds increment.

2026-07-29  docker-headless: gate timeline recording out of the headless
            entrypoint. --record-timeline (GNW_TIMELINE_RECORD) landed in
            the entrypoint alongside the feature, but it records LIVE
            keystrokes off the GPIO device's QEMU input handler, which
            only sees events a display backend delivers -- and the
            entrypoint hardcodes -display none, so no event ever arrives
            and the script would come out empty. There is also no way to
            watch a container's video live, so there is nothing to time
            presses against: the feature is structurally GUI-only. The
            flag now errors out pointing at the GUI rather than silently
            producing nothing, and headless-capture.md documents the
            record-with-a-window / replay-without-one split (the feature
            was previously undocumented anywhere). Replay is untouched --
            verified with a timeline+screenshot run.

2026-07-29  docs: record that upstream's test suite has gone unrun since the
            fork and should go into CI. `meson test --suite qtest-arm`
            gives 28 ok / 10 fail / 3 timeout on gnw-h7b0; the failures
            are all in boards this fork does not touch and predate the
            MPU fix (verified by running the same suite with that fix
            stashed -- identical results bar the new MPU test). Logged in
            docs/STATUS.md under both Known issues and Now/next, with the
            triage-then-baseline plan, since a permanently-red CI job
            would just get ignored.

2026-07-29  armv7m: stop an UNPREDICTABLE MPU region write from manufacturing
            a no-access region. Writing MPU_RBAR with VALID=1 (or MPU_RNR)
            naming a region the CPU does not implement is architecturally
            UNPREDICTABLE; QEMU discarded the write but left MPU_RNR
            pointing at the previously selected region, so the MPU_RASR
            write that HAL_MPU_ConfigRegion() issues next -- which takes
            its region number solely from MPU_RNR -- landed on that
            unrelated region and enabled it at its reset base of 0. A
            retro-go SD-card build passes an unaligned &_stack_redzone
            straight through as the region base, so RBAR's low 5 bits are
            accidentally VALID|REGION=8; that boots on real hardware but
            took a MemManage fault in gwemu, because region 2 came up
            enabled at address 0 with AP=0/XN=1 and itc_malloc()'s first
            allocation is address 0. The offending RASR write is now
            discarded too. Fixes a divergence found integrating gwemu
            into retro-go's workflows; repro material in backup/memfault/.
            This is stock upstream code (unchanged since 2017), so the
            bug is upstream QEMU's, not fork-local. Covered by a new
            tests/qtest/armv7m-mpu-test.c against mps2-an385, verified to
            fail without the fix.

2026-07-27  build: enable LTO on the shipped aarch64 Linux release asset.
            release.yml's build-linux aarch64 leg builds natively, not
            via contrib/docker-arm64-cross/build.sh, so that recipe's
            --enable-lto never reached the AppImage users download; the
            matrix now carries a per-arch extra_configure passing
            --enable-lto --disable-sdl --disable-sdl-image there, plus a
            b_lto assertion against meson-info so a silently non-LTO
            asset can't ship again. Measured +8.0% guest fps on a
            Cortex-A72 (an earlier +11.8% did not reproduce). Left off
            deliberately, with the reasoning recorded in each file, on
            x86_64 Linux (measured null, and would cost ui/sdl2.c),
            macOS, the static Windows exe, docker-headless and
            build-check.

2026-07-27  build: make libpng non-optional and its absence loud. configure
            now hard-errors when libpng is missing (fork-local check next
            to the png dependency in meson.build) instead of silently
            #undef'ing CONFIG_PNG and shipping a build whose screendump
            -- and hence the headless timeline's `screenshot` action and
            every screenshot-based benchmark comparison -- fails only at
            run time; --disable-png is the deliberate opt-out. Added
            libpng-dev to build-check.yml, dropped the --disable-png that
            shipped the 0.0.14 universal DMG unable to screenshot, and
            documented the requirement in CLAUDE.md.

2026-07-26  tcg: exempt the three TB-dispatch hot functions from compiler
            hardening. helper_lookup_tb_ptr(), curr_cflags() and
            arm_get_tb_cpu_state() run once per indirect guest branch --
            ~8M times/sec under retro-go's tgbdual, where perf attributes
            ~47% of the vCPU thread to them. The cost is almost entirely
            C-ABI and code shape, not algorithm: ~24% of the helper is
            prologue (a 144-byte frame with four callee-saved register
            pairs spilled on the fast path), and QEMU's default hardening
            is disproportionate at that call rate --
            -fstack-protector-strong adds a GOT-indirect canary load,
            store and check, -fzero-call-used-regs=used-gpr adds ten
            register-clearing stores per return. Exempting just these
            three measured +7.9% guest throughput on a Raspberry Pi 4
            (~75% of the +8.1-10.6% available from dropping both flags
            binary-wide), with hardening left on everywhere else: 7344
            stack-canary sites remain in the binary and none are in the
            three functions. Neither attribute changes semantics. Routed
            through QEMU_HOT_NO_HARDENING in include/qemu/compiler.h,
            __has_attribute-guarded so clang and older GCC simply keep the
            hardening rather than failing the build.

            Measured, and rejected: the jump cache is NOT the problem
            (98.84% hit rate, better than SMW's 98.0%; 2K entries drops it
            to 74%, 16K is neutral, 64K pathological). tgbdual is
            expensive because 41.2% of its TB exits are indirect versus
            SMW's 12.6% -- an interpreter dispatching every opcode through
            a table -- so it pays 3.3x more lookups per unit of work. None
            of those exits are chainable-but-unchained; the targets are
            data-dependent by construction. LTO was bimodal and not worth
            the build cost; devirtualising get_tb_cpu_state gained 1.3%;
            noinline on the slow path gained 0.6%. Upstream has nothing to
            adopt: tb-jmp-cache.h and tb-hash.h are byte-identical between
            our pinned v11.0.2 and current master.

2026-07-26  ui: GNW_VBLANK_HZ, a host-side vblank pump rate knob. The GUI
            paces process_vblank() -- framebuffer blit plus
            dpy_gfx_update() -- at a fixed 60Hz. Raising it to 120Hz
            measured +25.6% guest frame rate on 60Hz content on a
            Raspberry Pi 4 (37.9 -> 47.6 GUESTFPS), and the identical
            +25.6% on both a 60Hz and a 120Hz panel, so it is
            frame-delivery latency rather than panel alignment: total BQL
            hold time is unchanged, but splitting it into smaller, more
            frequent slices means a ready guest frame waits ~8.3ms instead
            of ~16.7ms to be published. Above 120Hz gains nothing -- only
            ~42 pumps/s ever find new guest content and the rest are
            sub-50us no-ops, which dilute the mean hold without shrinking
            the real work (measured flat from 120 to 720Hz). Host-side
            only: the guest-visible LTDC vblank is a separate
            QEMU_CLOCK_VIRTUAL timer derived from PLL3R, and an 8x change
            here moves guest LTDC ticks/s by under 1%. The default is
            unchanged for now, pending the same measurement on other
            hosts and on 30fps-capped content.

2026-07-26  build/ui: stop defining bare DEBUG across the whole vendored
            SDL3 build. meson builds SDL3 as a CMake subproject and, since
            QEMU's own meson buildtype is "debug", meson's cmake module was
            passing -DCMAKE_BUILD_TYPE=Debug; SDL3's CMakeLists.txt turns
            that into target_compile_definitions(... $<$<CONFIG:Debug>:DEBUG>),
            so every SDL3 object was compiled with -DDEBUG. Pass an explicit
            (empty) CMAKE_BUILD_TYPE instead -- meson only appends its own
            when we have not supplied one, so ours wins -- which drops
            -DDEBUG without adding anything in its place. Verified against
            `ninja -t commands`: the only change to the SDL3 compile lines
            is -DDEBUG going away; -O2 is unchanged and the generated
            SDL_build_config.h is byte-identical. (CMAKE_BUILD_TYPE=Release
            also works but silently swaps in -O3 -DNDEBUG, so it was not
            used.)

            The visible consequence was that every release shipped with
            SDL's internal debug assertions live: SDL_assert.h picks
            SDL_ASSERT_LEVEL 2 straight off `defined(DEBUG)`. A user on a
            Raspberry Pi (PulseAudio-on-PipeWire) hit the SDL_assert in
            SDL_AddAudioDevice() on a duplicate device handle; SDL answers
            an assert with a modal dialog, which on Linux is an external
            zenity process that outlives the window and blocks the audio
            hotplug thread that SDL_Quit()'s teardown then waits on -- so
            gwemu never exited, while still holding the flash images mapped
            WRITABLE, and the next launch was blocked. libSDL3_static.a
            drops 35.3MB -> 34.2MB and the assertion string is now absent
            from both it and qemu-system-arm.

            Scope of what DEBUG actually gated, on the platforms we build:
            only the assert level. SDL_internal.h's use is behind
            SDL_DISABLE_INVALID_PARAMS (not set) and SDL_malloc.c's
            dlmalloc consistency checks are behind #ifndef HAVE_MALLOC
            (HAVE_MALLOC is 1). Everything else matching "DEBUG" in the
            tree is a distinct macro (DEBUG_JOYSTICK, DEBUG_XEVENTS, ...)
            that -DDEBUG does not enable. So this is correctness/hygiene,
            not a performance fix.

            Belt and braces in ui/gwemu.c: set SDL_HINT_ASSERT to "ignore"
            before SDL_Init unless the user set SDL_ASSERT, so a non-fatal
            internal inconsistency degrades to a log line rather than a
            blocking modal on any platform -- also covering a locally built
            debug SDL and SDL_assert_release(). SDL_ASSERT=abort restores
            the debugging behaviour.

            The duplicate registration itself is an SDL3 upstream issue,
            not ours: the pipewire backend derives handles from recyclable
            node ids while device removal is asynchronous, so a node
            re-announce can collide with a zombie device still holding that
            handle. Nothing here calls SDL_AddAudioDevice. The vendored
            SDL3 tree is deliberately left unpatched.

            NOT an issue, contrary to a first diagnosis: SDL3 is not
            shipped unoptimised. meson's cmake.subproject() does not hand
            compilation to CMake -- it runs CMake configure-only and builds
            the extracted sources as native meson targets under
            optimization=2. All SDL3 objects carry -O2, none carry -O0;
            CMakeCache.txt's CMAKE_C_FLAGS_DEBUG="-g" describes a configure
            step that compiles nothing.

2026-07-26  ltdc: fix an intermittent SIGSEGV on slow hosts (seen on the
            Raspberry Pi 4, windowed only). gnw_h7b0_ltdc_fb_range_dirty()
            checked section->mr, then re-read it after calling
            memory_region_snapshot_and_clear_dirty() -- but that call is
            not BQL-atomic: it reaches do_run_on_cpu(), which
            qemu_cond_wait()s on qemu_work_cond and releases the BQL. In
            that window the vCPU thread services a guest LTDC register
            write whose VBR teardown sets section->mr = NULL, and the main
            loop resumes to pass NULL into
            memory_region_snapshot_get_dirty(). Confirmed with a hardware
            watchpoint naming the writing thread and stack, and with
            breakpoints showing the pointer flip inside the call. Latch mr
            and offset before the snapshot, hold a real reference for its
            duration (otherwise the NULL deref is a use-after-free), and
            report dirty if it changed on return -- the function's own
            "not provably clean is dirty" rule. Windowed-only because
            -display none never drives the fallback capture path; Pi-only
            because four slow cores make the vCPU far likelier to be
            parked on the BQL at that instant. Repro 7/13 before, 0/12
            after (Fisher p ~ 0.0006); an intermittent race, so absence
            cannot be proven.

2026-07-26  ui: finish the SD-card creation feature -- pick a folder, get a
            card. The SD Card tab (previously compiled but never
            registered in MainMenuScene, so unreachable) is now a real tab
            after Profiles. It calls gnw_sdimg_build() in-process via
            gwemu_sdcreate_qcow2_progress() instead of popen()-ing
            "python3 scripts/make_sdcard_image.py" against a
            working-directory-relative path -- that shell-out, its
            ShellQuote() helper and the hardcoded
            backup/qemu-images/sdcard.qcow2 output are gone, along with
            the whole class of bug it caused for installed/portable
            builds. Output lands in the shared-SD registry
            (<app data>/sd-cards/<name>.qcow2 + a shareable=true sidecar),
            which is exactly what the profile wizard's "Shared" SD picker
            enumerates, so a card built here is attachable from any
            profile; an opt-in checkbox also points the active profile at
            it directly. The folder picker is a folder picker now (the
            actual feature gap), the content folder is scanned on a worker
            thread for a real size/count readout and a fits/doesn't-fit
            check against the chosen card (capacity math mirrors
            gnw_fat32.c's own cluster table and FAT-size formula, so it
            cannot disagree with the builder), the size preset auto-steps
            up to the smallest one that fits, and the build shows real
            byte-level progress from the sector-write callback rather than
            a static "this can take a few minutes". Colors/spacing route
            through gnw-style-tokens.hh; icons stay inside the compiled
            55-glyph FontAwesome subset. gwemu-sdcreate.c now takes the
            BQL per block-layer call instead of once around the whole
            build: blk_pwrite() needs it held (AIO_WAIT_WHILE's
            home-thread test for the main AioContext is literally
            bql_locked()), but holding it across a multi-GB content copy
            would freeze the UI and the machine for minutes -- the exact
            failure this feature caused before. New settings:
            general.sdcard.{content_dir,size_index}, saved on change.

2026-07-26  blank internal flash is a supported state, not a crash. A guest
            with no valid vector table faults immediately, cannot escalate
            to HardFault at priority -1, and locks up -- and armv7m_nvic.c
            handled v7M Lockup with cpu_abort(), which abort()s the whole
            process. So binding a blank or missing bank1 killed the GUI on
            every start, before it was usable. That is backwards: blank
            flash is exactly what real hardware looks like before it has
            been flashed, and gnwmanager/GDB attaching to flash the device
            is a workflow we support. Lockup now models the silicon
            instead: clear the stuck exception state, drop the NVIC line,
            clear the latched CPU_INTERRUPT_HARD and env->event_register
            (all four needed, or arm_cpu_has_work() stays true and the
            vCPU spins a full core re-faulting), halt, and re-reset the
            machine every 250ms off a QEMU_CLOCK_VIRTUAL_RT timer.
            Measured: 0.8% of one core over 30s, flat RSS, one log line
            total. The loop stands down under RUN_STATE_DEBUG and a
            vm-state-change handler unhalts and kicks the vCPU on resume,
            which is what lets a debugger load a loader into RAM and
            actually run it -- verified with a real gdb client: attach,
            system_reset, memory read/write, a 4-instruction Thumb loader
            written to RAM and single-stepped, and breakpoint + continue.
            (Whoever drives this must set the xPSR T-bit; writing PC alone
            leaves thumb at its reset value and the code faults at once.)

            Alongside it, the profile image-copy path could destroy an
            image it failed to replace: copy_padded() opened the
            destination "wb" -- truncating the live file before reading a
            byte of the source -- and g_unlink()ed it on error, so an
            interrupted re-bind (including one interrupted by the
            Flash-Apply execv() restart) left the profile with no bank1
            and no way back. QEMU then silently materialised the slot via
            memory_region_init_ram_from_file()'s ftruncate, which is where
            the all-zero, fully-sparse 256K bank1 that prompted all this
            came from. Copies now write a temp file and rename on success
            only, so a failure never touches the existing image, and an
            empty source is rejected rather than written as a pad-only
            image. The SoC reports image creation/extension as info rather
            than passing it over in silence.

2026-07-26  ltdc: publish once per guest frame, not twice. Stock firmware
            swaps buffers with two SRCR writes ~5us apart -- SRCR=IMR
            (HAL_LTDC_SetAddress's immediate reload, which already flips
            L1CFBAR to the other buffer) then SRCR=VBR -- and the model
            captured, composited, blitted and dpy_gfx_update()'d on both,
            for two bit-identical images. Traced on a 29s Super Mario
            World run: 1876 captures for ~870 guest frames, 46 device
            publishes/s against a 30fps guest. On real hardware the
            second reload latches nothing (the shadow registers are
            unchanged since the immediate one) and the panel keeps
            scanning out one image, so this was a modelling artifact, not
            two real frames. The IMR path now skips its capture when a
            VBR reload has arrived since the previous IMR (i.e. this
            firmware is VBR-paced, so the VBR write microseconds later
            will capture the same configuration) and the reload isn't
            structural; guests that double-buffer with IMR alone, and the
            HAL's format/geometry reloads, are unaffected. Measured, 3
            runs each: captures 1870-1876 -> 949-966, device blits/s
            46.3-47.6 -> 30.3, publishes/s 44.6-45.8 -> 30.5-30.7 (the
            guest rate exactly). GUESTFPS unchanged at 29.98 windowed
            and headless. This also retires an earlier UI-side workaround
            for the same bug -- gwemu_update_fb_texture used to memcmp
            the guest surface against the bytes already staged and drop
            byte-identical republishes; with the device publishing once
            per frame its hit rate collapsed from 15.7-16.7/s to 1.6/s,
            i.e. a full-surface compare under the BQL on ~95% of frames
            to catch ~5%, so it is gone. The per-second "UI gate:" trace
            line stays (publishes and admission reason), minus its dup
            field. Validated together with the content gate below on a
            Pi 400 (Super Mario World attract demo under retro-go,
            interleaved A/B against the tip of the branch, 7 valid
            "before" and 4 valid "after" windowed runs, 4+4 headless):
            windowed GUESTFPS 26.28 [25.72-27.08] -> 28.45 [28.34-28.62],
            renders/s 118.0 -> 29.0, texture uploads/s 59.7 -> 29.0,
            SDL_RenderPresent 716ms/s -> 19ms/s, HUD 33ms/s -> 8ms/s, and
            CPU 157.8% -> 118.7% in-process plus Xorg 31.1% -> 8.1% and
            the window manager 10.7% -> 2.5%, i.e. 199.7% -> 129.3% of a
            core system-wide. Headless is unregressed at the 30fps cap
            (GUESTFPS 29.77 -> 29.83, 106.2% -> 100.7%). retro-go's own
            menu and its modal dialog were confirmed to still render
            correctly -- neither takes the new IMR path.

2026-07-26  GUI: render only when there is something new to show. The
            render loop was free-running at the host refresh rate
            (measured 119.7 presents/s here, 117-121/s on a Pi 400) for a
            guest producing 30-60 frames/s, and m_fb_dirty was set once
            per 60Hz vblank pump rather than per actual guest frame, so a
            30fps guest still did 60 texture uploads/s. Two fixes: the
            dcl now registers a real dpy_gfx_update op, so the dirty flag
            means "the LTDC published a new frame"; and a content gate in
            gl_render_frame renders only on new guest content, with a
            60Hz ceiling while the UI is busy (settings window visible,
            input within the last 400ms, or ImGui wanting kbd/mouse) and
            a 10Hz heartbeat otherwise so time-driven HUD animations
            still advance. It composes with the occluded-window throttle
            (probes bypass the gate). Windowed: 119.7 -> 60.1 presents/s
            and 985ms -> 3.9ms/s spent inside SDL_RenderPresent, with
            guest fps and timeline wall time unchanged. The vblank pump
            thread also stopped using SDL_DelayPrecise, whose busy-spin
            tail bought accuracy nothing downstream needs. Headless
            unchanged (45.02s wall, 59.6 GUESTFPS, 3 runs). On a Pi 400
            this is the larger half of the combined saving above: nearly
            all of the 697ms/s of SDL_RenderPresent and of the 23-point
            Xorg drop comes from no longer presenting 118 times a second.

2026-07-26  GUI: GDB stub settings (System tab). Enable/disable, listen
            address and port, persisted as sys.gdb.* and applied at
            runtime via gdbserver_start() -- "none" is the supported
            teardown (it destroys the socket chardev), so the toggle
            takes effect immediately with no restart and no leaked
            listener. Startup application is deferred to the first HUD
            frame with a CPU, since the HUD initialises before machine
            init. Address defaults to loopback: the stub is
            unauthenticated full guest-memory access.

2026-07-26  RCC/DMA/SAI1: model peripheral resets, eliminating the DMA
            half of the retro-go quit-to-main-menu black screen. The
            2026-07-26 entry below reduced it 9.1% -> 3.0% but could
            not explain the rest. The rest was this: retro-go calls
            HAL_DeInit() on the way out of a game, which (sdk/.../
            stm32h7xx_hal.c) does __HAL_RCC_<bus>_FORCE_RESET() /
            RELEASE_RESET() for all nine buses. We modelled every
            xxxRSTR write as inert, so the game's still-armed SAI1/DMA1
            audio stream survived into the freshly booted launcher --
            whose HAL handles are zeroed .bss -- and the first
            half-transfer interrupt it raised could never be
            acknowledged, latching the IRQ line high forever.
            RCC writes to AHB1RSTR/APB2RSTR now device_cold_reset() the
            registered devices on a 0->1 edge (asserting the whole
            reset on the rising edge is indistinguishable from outside,
            since every HAL user sets and immediately clears the bit).
            Confirmed on real hardware before writing the fix: 99,287
            samples read over SWD without halting the CPU showed
            DMA1_LISR == 0 in every one -- silicon never sits with a
            flag latched -- and S0CR dropping to 0x00000000, its reset
            value, at several quit points. Measured across 237
            interleaved runs on two hosts: 24/118 storms before,
            **0/119** after (Fisher exact p = 1.5e-8). Audio survives
            the reset (HT/TC pairs continue through quit and re-entry).
            The failure rate tracks HOST SPEED, exactly as the user
            observed: idle Linux 3%, loaded Linux 17%, an older Intel
            Mac 34% -- the vulnerable window is a fixed number of guest
            instructions while our DMA events are paced by wall time,
            so a slower host widens it.
            STILL OPEN, a SECOND and independent bug with the same
            symptom: the CPU can instead wedge in the LTDC interrupt
            (exception 104 / IRQ 88) with DMA1 clean and at its reset
            value. Unaffected by this fix (68% vs 65% of non-storm
            runs). LTDC sits on APB3 and HAL_DeInit() resets APB3RSTR
            bit 3 (LTDCRST), which we deliberately do NOT model yet --
            it is unverified and an LTDC reset would blank live display
            config, so it needs its own measurement.

2026-07-26  build: true aarch64 cross-compile (contrib/docker-arm64-cross/),
            replacing the emulated qemu-user binfmt recipe as the primary
            way to build for a Raspberry Pi from an x86_64 dev box. The
            compiler now runs natively and emits ARM, so a full clean
            build is ~1m45s instead of ~50 min. Debian base (one mirror
            for all architectures) + `dpkg --add-architecture arm64` +
            the `:arm64` dev packages, with PKG_CONFIG_LIBDIR pinned to
            the aarch64 multiarch dir so pkg-config can't see host .pc
            files. No hand-written CMake toolchain file was needed:
            meson's cmake module derives one from configure's
            config-meson.cross, so the SDL3 subproject cross-builds and
            the result is a full GUI build (no GNW_ALLOW_NO_GUI).
            Verified on a real Pi 4 running Celeste under retro-go.
            docs/cross-platform-builds.md rewritten accordingly; the
            emulated method stays documented as a fallback.

2026-07-26  DMA/RCC/SAI1: largely fixed the long-standing intermittent black
            screen on retro-go's "quit to main menu". The DMA1 Stream0
            (SAI1 audio) IRQ line latched high forever and the CPU
            tail-chained exception 27 (IRQ 11) for the rest of the
            session; the display was a victim, not the cause. Root
            cause: retro-go reprograms PLL2 -- which per the HAL's
            RCCEx_PLL2_Config() means switching PLL2 *off* for ~1.4ms
            -- a couple of milliseconds before it disables the stream
            and clears its flags, and its ISR no longer acknowledges
            anything in that window. Real hardware raises nothing there
            (no SAI kernel clock -> no DMA requests -> the stream makes
            no progress at all), but this model paced HTIF/TCIF off a
            timer keyed only on SxCR.EN and manufactured an interrupt
            firmware could never clear. Three parts: RCC now reports
            pll2_p_ck as 0 while CR.PLL2ON is clear; the DMA controller
            gained an optional per-stream "is the peripheral actually
            requesting?" predicate (GnwH7B0DmaStreamActiveFn) which
            stalls the stream and slides its deadline instead of
            raising a flag; and SAI1 supplies that predicate and now
            keeps its DMAMUX request registration for the life of the
            machine rather than tearing it down on SAIEN 1->0 (that
            unbinding conflated the host audio voice with firmware's
            DMAMUX routing and left the stream free-running at the
            generic 48kHz fallback during exactly the teardown window
            that matters). Measured headlessly with an automated
            quit-to-menu timeline, legacy and fixed runs interleaved in
            the same parallel batches so host load is identical:
            12/132 black screens before, 4/132 after. Reduced, NOT
            eliminated -- the remaining ~1.7ms of the danger window
            runs from PLL2 coming back up to firmware writing
            SxCR.EN=0, and in that sub-window our model does what real
            hardware appears to do (SAI enabled and clocked, so a
            half-transfer interrupt is legitimately due), so whatever
            still differs there is not yet identified. Do not read a
            single clean run as a fix; the base rate is only ~10%.
            New GNW_DMA_TRACE={1,2} diagnostic (per-second summary /
            per-event) for DMA1 stream 0.

2026-07-26  Build: documented a working aarch64-under-binfmt container
            build (docs/cross-platform-builds.md). Three dependencies
            fail misleadingly: liblzma-dev (gnw-tools hard-errors, but
            after SDL3's configure output so it reads as an SDL3 bug),
            PyYAML in the build dir's venv (genconfig; not vendored in
            python/wheels), and libpng-dev (its absence silently
            #undefs CONFIG_PNG, so screendump refuses to write PNGs and
            headless screenshot validation breaks while the build looks
            fine -- and ninja will not re-probe, so it needs
            `meson setup --reconfigure`). Verified by building and
            benchmarking on a real Pi 4: holds full realtime, 29.9fps
            steady-state on the Celeste bench.

2026-07-26  Found and fixed the in-game macOS/Windows slowdown that the
            2026-07-25 entry left open: off Linux, QEMU's main loop
            cannot wait for less than a millisecond. qemu_poll_ns()
            uses nanosecond ppoll() only under CONFIG_PPOLL, which
            macOS and Windows lack; they fall through to g_poll() with
            a timeout that qemu_timeout_ns_to_ms() deliberately rounds
            UP, so every sub-millisecond timer deadline overshoots.
            The guest is ~92% idle in gameplay, so its frame rate is
            set by wakeup promptness, not throughput: 2095 main-loop
            wakeups/s on Linux vs 1223/s on Windows, and the guest then
            renders two thirds of its frames. Emulated vblanks fired at
            the same rate on both hosts (~1720 in 29s), which is why
            this hid behind a healthy-looking display timer. Measured
            on Celeste across five machines with one binary per host
            and an A/B env var: Win11 VM 20.0 -> 30.0fps, Win11 laptop
            24.7 -> 30.0 AND 53.7s -> 29.1s wall for a 29-guest-second
            run (55% of realtime -> realtime), macOS 27.1 -> 30.0;
            both Linux hosts already sat at Celeste's 30fps cap and
            were unaffected. The VM and its own Linux hypervisor share
            a physical CPU (20.0 vs 30.0), so this is not CPU age.
            macOS fix: qemu_pselect_ns() in util/qemu-timer.c, whose
            struct timespec is honoured at ns resolution -- +7% CPU for
            +3fps. Windows fix: qemu_timed_wait_ns(), which appends a
            cached CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer (100ns)
            to the wait set and lets g_poll() wait forever -- the timer
            firing IS the timeout, so glib's win32 handle/message
            semantics are untouched and only the rounding goes away
            (Winsock select() handles sockets only, so the macOS cure
            does not port); +34% CPU for a 20 -> 30fps recovery. Both
            beat the GNW_POLL_SPIN diagnostic that proved the cause,
            which cost 3.8-4.4x the CPU for identical fps. New env vars
            GNW_POLL_SPIN (diagnostic, spins -- never a default) and
            GNW_POLL_MS_ONLY (forces the old rounding back to A/B).
            Also learned: ranking hosts by fps alone is wrong, since a
            struggling host either drops frames at realtime or lets the
            virtual clock lag -- always report wall time for a
            fixed-length timeline too. See docs/emulation-performance.md.

2026-07-25  Emulation performance: measured why MMIO-heavy firmware
            paths are slow, and why macOS/Windows suffer more. The
            retro-go launcher issues ~2.3M JPEG register reads/sec;
            QEMU forces each MMIO access to be its translation block's
            LAST instruction, so every one takes cpu_io_recompile()
            (tree lookup, state unwind, longjmp, one-insn TB) and that
            decision is never cached -- 2.68M recompiles/sec, ~60% of
            wall time (skipping it: 53 -> 129fps in the launcher).
            Verified on the real device that the polling is authentic
            firmware behaviour, not a modelling bug: no MDMA channel is
            ever enabled and the JPEG codec sits at 22% duty
            (gnwmanager's OpenOCD backend reads device memory without
            halting the CPU). Four fixes tried and rejected on
            measurement: blocking SR poll, one-insn-per-tb (53->19fps),
            skipping the recompile (incorrect -- loses interrupt
            precision), and caching which PCs do I/O (recompiles -29x
            but only +5-10% fps; shorter blocks cost back the saving).
            Platform costs quantified: macOS pthread_mutex is 8.1x
            Linux's (18.7 vs 2.3ns) and QEMU takes the BQL once per
            MMIO access; macOS additionally pays _longjmp (4.5% of
            samples -- Darwin saves the sigmask) and _tlv_get_addr
            (2.0% -- TLS via a dyld call) in that same path. New
            diagnostic env vars (GNW_MMIO_PROF, GNW_BQL_PROF,
            GNW_IDLE_PROF, GNW_IORECOMP, GNW_MMIO_OFF, GNW_JPEG_LAT,
            plus GUESTFPS/DMALATE under GNW_UI_FRAME_TRACE) and the
            measurement pitfalls that produced several wrong
            conclusions along the way are in
            docs/emulation-performance.md. STILL OPEN: a 10-17% in-game
            deficit on macOS/Windows that none of the above explains.

2026-07-25  Render path: the framebuffer texture is now uploaded only
            when the guest has actually redrawn (dirty flag set from
            graphic_hw_update), and the GPU upload itself moved OUT of
            the BQL -- the lock only covers a memcpy into a staging
            buffer now. BQL contention from rendering fell 130ms/s ->
            1.7ms/s (98%). On a 120Hz host driving a ~59Hz guest this
            also halves uploads (120/s -> 60/s), which was pure waste.
            Confirmed by ear on Linux across several games. Found by
            instrumenting both lock regions in gl_render_frame after
            the user observed that hiding the window made emulation
            speed up -- the existing "FIXME: Don't upload if notdirty".

2026-07-25  Windows: gnwmanager patch-binary download rewritten
            in-process (ui/gwemu-http.c, WinINet). The popen("curl ...")
            chain could never have worked there -- POSIX 'quoting',
            /dev/null, and a wget fallback the static build doesn't ship
            -- and each attempt also flashed a cmd.exe console window
            over the GUI. Both symptoms reported from a real Windows 11
            run of the profile wizard; fixed together, since removing
            the child process removes the window. Verified on Windows 11
            (downloads the mario patch binary byte-identically,
            sha1 b70cd02e..., and reports real HTTP status on failure).
            WININET.dll added to release.yml's self-contained-exe
            whitelist.

2026-07-25  Release pipeline: Linux arm64 (Raspberry Pi 4/5) + two real
            packaging bugs. build-linux is now a matrix shipping
            gwemu-<v>-x86_64.AppImage and gwemu-<v>-aarch64.AppImage,
            built natively on ubuntu-24.04-arm (free aarch64 runner);
            build-appimage.sh picks ARCH from uname -m; build-check.yml
            covers arm64 too. Validated end to end on a real Pi 4:
            boots stock Mario headless with no SD card, steady ~59.6fps
            (1800 vblanks in 30423ms), ~41% of one core. Fixes found by
            testing rather than review: libpng was missing from ALL
            THREE release legs, so the shipped binaries could run a
            headless timeline but never capture from it ("Enable PNG
            support with libpng for screendump" -- confirmed on the Pi
            and on the 0.0.14 macOS universal DMG).

2026-07-24  GUI: device-profile system Phases 1-2 + staged firmware wizard.
            Profile store backend (ui/gwemu-profiles: per-profile dirs
            with owned flash copies + profile.toml via tomlplusplus,
            shared-SD registry, docker-style name generator). CFW patch
            driver librarified (gnw_cfw_build_images -- GUI patches
            in-process, popen path deleted); gnwmanager per-game patch
            binaries lazily downloaded (hardcoded raw-file URLs, sha1
            spot-verified, appdata cache). Two-stage wizard designed by
            a multi-agent UI team (flow + visual specs, merged/arbitrated)
            replaces the xemu welcome window entirely, hosted in the
            settings window: Add Firmware stage (Mario/Zelda status cards
            with tactful illumination, folder unit) -> New Device Profile
            form (template radios, Bank Assignments accordion, unified
            Patched checkbox default-on, power-of-2 extflash stepper with
            a shared ZeldaInvolved() 4MiB floor, SD Card section --
            import/attach live, create gated). Launch = relaunch flow,
            now carrying -drive if=sd. New C SD-image builder
            gnw-make-sd-image (MBR+FAT32 behind a sector-write callback;
            fsck-clean, mtools tree-identical to the Python oracle which
            is now test-only). Fixes en route: -config_path argv
            compaction segfault + relaunch argv capture ordering,
            FontAwesome merged into default/small fonts (MergeMode
            attaches per-font -- '?' glyph boxes), dialog defaults
            absolutize-or-NULL, pinned-row margins, occluded-main-window
            render stall (event-driven adaptive present throttle;
            compositor never reports SDL occlusion here -- self-clocking
            50ms/2-strike detection, event-driven resume, 5s failsafe
            probe, GNW_UI_FRAME_TRACE tracer).

2026-07-24  RELEASES: single-file-per-platform asset lineup. Linux:
            gwemu-<ver>-x86_64.AppImage (contrib/appimage/, verified
            locally against a real firmware boot; fully-static Linux
            ruled out -- GPU userspace is runtime-loaded per-hardware).
            macOS: ONE universal gwemu-<ver>-macos-universal.dmg --
            per-arch self-contained gwemu.app bundles (recipe validated
            on the real Intel iMac incl. the mandatory dylibbundler
            LC_RPATH dedup -- modern dyld aborts on duplicates)
            lipo-merged binary+dylibs pairwise by a new CI job that
            fails on any arch dylib-set drift and smoke-tests the
            arm64 slice. Windows portable
            renamed gwemu-portable.exe. All direct un-zipped assets;
            no more platform zips, no qemu-system-arm assets (the
            single binaries accept explicit -M/-display args); gnw
            scripts ship once as a separate optional scripts.zip.

2026-07-24  WINDOWS: single-file static gwemu.exe + installer, and the
            console-flood freeze. Adopted xemu's distribution model via
            its public MXE static toolchain image plus a static liblzma
            (contrib/docker-win-static/): gwemu.exe now links glib/
            pixman/everything statically, imports only Windows system
            DLLs (~42MB stripped), replacing the 38-DLL dist folder.
            Releases ship it and gwemu-setup.exe (new NSIS per-user
            installer, contrib/gwemu-installer/) as direct un-zipped
            assets; Windows gets no scripts/ or qemu-system-arm.exe.
            CI enforces self-containment via an objdump import
            whitelist. Field-debugged en route: the "static exe
            freezes" report was per-frame GNW_* trace output flooding
            an attached Windows console (console I/O blocks; redirect
            to a file and it ran perfectly) -- the probes were stuck on
            because bare getenv() presence checks treated GNW_FOO=0 as
            enabled. gnw_env_enabled() now treats unset/empty/0 as off,
            start.sh no longer exports the probes, and the framebuffer
            texture path logs its failures instead of dying silently
            (fb_texture: lines). Both artifacts verified working on
            real Windows (portable exe and installer). Verified the
            hard way that wine is useless for testing this (~1 frame
            per 10-15s) -- docs now say so.

2026-07-24  CI/BUILD: three release-asset fixes from the v0.0.10 field
            reports. (1) Linux/Mac zips shipped non-executable binaries:
            upload/download-artifact@v4 strips permission bits, so the
            build jobs' chmod +x never survived to publish-release; exec
            bits are now restored there right before the final zip.
            (2) Windows gwemu.exe opened a console window: the meson
            alias byte-copies console-subsystem qemu-system-arm.exe;
            scripts/make-gwemu-alias.py now flips the copy's PE subsystem
            to GUI (qemu-system-arm.exe stays console for -M help etc.).
            (3) The v0.0.10 macOS assets shipped with NO GUI at all: the
            SDL3 cmake subproject failed on the mac runners and its
            required:false let configure finish silently (gwemu behaved
            like plain upstream QEMU -- no -M injection, no window).
            meson now hard-errors when SDL3 fails unless GNW_ALLOW_NO_GUI=1
            is set; the runner-side SDL3 failure itself is still to be
            diagnosed from the next CI run's configure output.
            Also field-diagnosed as NOT bugs: "menus stopped appearing"
            was Tab's toggle-and-persist of display.ui.show_menubar in
            gwemu.toml (affects every binary equally); the Linux CI
            binary benchmarks identical to a local build.

2026-07-24  PERF: JPEG device model decode/poll overhaul (the "retro-go is
            slow on Windows" hunt, which ended somewhere else entirely).
            (1) The naive fdct/idct called libm cos() in their innermost
            loops -- 8192 calls per 8x8 block, measured at 36% of ALL
            process cycles under launcher scroll; now a precomputed
            8x8 cosine table plus a sparse, hoisted restructure of both
            transforms that preserves float evaluation order exactly
            (bit-identical output, verified via decode-hash traces).
            ~5KB cover thumbnail: 8.0ms -> 0.43ms. (2) Inputs <=256KB now
            decode synchronously inline at EOI (worker thread retained
            for larger), so results are published before firmware's first
            status poll -- eliminates the SR wait-poll storm (was ~40% of
            launcher JPEG MMIO). Fixed a pending-pointer double free the
            inline path exposed in the publish handoff. (3) Lock-free
            atomic decode_done check replaces a mutex trylock/unlock pair
            that ran on every register read (~12% of vCPU cycles at the
            measured ~2-4M reads/s). (4) All env-var trace checks in
            emulation-hot paths now resolve getenv() once and cache --
            msvcrt's getenv is a locked linear scan, and one per-CNT-read
            check in TIM2 measurably slowed whole-guest execution on
            Windows. Findings worth keeping: retro-go's launcher AND the
            stock-side zelda3/GB games are JPEG-MMIO-throughput-bound on
            every host (fps tracks register-access rate; ~2M/s Windows VM,
            ~2.8M/s bare-metal Linux on faster silicon, ~4.4M/s laptop);
            two approaches tried and REVERTED with warnings left in code:
            blocking the SR poll (3x slower -- firmware overlaps work with
            the polled decode) and memory_region_enable_lockless_io (hard
            crash on real Windows). New env-gated instrumentation:
            GNW_TIMER_LATE (vblank/DMA dispatch lateness + TIM expiry
            counters), GNW_MMIO_RATE (JPEG register-read rate, per-offset),
            JPTDUR decode timing under GNW_JPEG_TRACE.

2026-07-24  Windows portability fixes: %lx -> HWADDR_PRIx/PRIx64 in RTC/
            TAMP trace printfs (LLP64 -Werror break), and the RTC/TAMP
            debug printfs themselves removed (unconditional per-MMIO
            console writes; conhost made them a real whole-emulator
            slowdown). Settings window opens 900x700 clamped to the
            desktop's usable bounds (was 1600x1200, larger than some
            screens with no reachable resize edge).

2026-07-24  start.sh: Linux counterpart to start.bat (same dual-boot image
            set), tees traces to gwemu.log, perf-probe env vars on.

2026-07-24  CI: release.yml's Windows job now uses the documented Docker
            cross-build (gwemu-win-cross image, --disable-sdl et al.,
            recursive DLL-walk dist packaging) instead of a divergent
            MSYS2 native build that still linked SDL2; new fifth
            build-docker job publishes contrib/docker-headless to Docker
            Hub as slashproc/gwemu-headless with a vX.Y.Z -> :X, :X.Y,
            :X.Y.Z, :latest tag cascade (REGISTRY_TOKEN secret).

2026-07-23  FEATURE: headless capture appliance (Docker/CI) -- true
            `-display none` support (windowless upstream-main path in
            ui/gwemu.c; previously every "headless" run still opened a dead
            SDL window and ran ~16x slow), a virtual-clock timeline script
            engine (hw/misc/gnw_timeline.c, GNW_TIMELINE=<file>: press/hold/
            release/screenshot/quit at [MM:]SS[.fff] guest time or @N vblank
            frames), a virtual-clock session recorder (hw/display/
            gnw_h7b0_recorder.c, GNW_RECORD/<fps>, raw frames + wav audiodev,
            ffmpeg-muxed to mp4/mkv/flac by the standalone
            contrib/docker-headless/ entrypoint+Dockerfile), and a
            synchronous PNG screendump helper (gwemu_screendump_png).
            Verified: byte-identical screenshots across independent runs at
            plain realtime (the repeatability headline), 1:1 wall/virtual
            pacing, GUI path unaffected. Measured and documented: -icount is
            NOT a speed-up button (fixed shift changes apparent guest CPU
            speed and results; shift=auto paces to realtime) -- see
            docs/headless-capture.md. Also fixed: the host had silently lost
            libpixman-1-dev, so CONFIG_PIXMAN (and QMP screendump with it)
            had been compiled out -- reinstalled and now load-bearing.

2026-07-23  PORT: OpenGL removed from the GUI entirely; SDL3 unifies rendering
            and audio on all three platforms. (1) Rendering now goes through
            SDL_Renderer -- D3D11 on Windows, Metal on macOS, Vulkan (then GL,
            then software) on Linux, with SDL's CPU rasterizer as universal
            fallback -- replacing the hard GL 3.2 Core context requirement
            inherited from xemu (which cost a real "Unable to create OpenGL
            context" fatal on a Windows 11 VM with QXL graphics). ImGui runs
            on imgui_impl_sdlrenderer3; the framebuffer is a streaming
            SDL_Texture; screenshots/thumbnails encode from guest pixels (no
            GPU readback); the GL SDF logo animation became a static image.
            (2) New "sdl3" audiodev (audio/sdl3audio.c, ported from the SDL2
            driver to SDL3's stream API): WASAPI/CoreAudio/PipeWire via the
            statically-linked SDL3 the GUI already ships, now the default-
            priority driver on every host. Replaces dsound (failed outright on
            a real Win11 VM) and QEMU's aging coreaudio backend (audibly
            better on the project's real Hackintosh). (3) Linux now defaults
            to the x11 video driver (XWayland) -- third real Wayland breakage
            (fractional-scale UI mis-sizing) joined the libdecor crash and the
            ImGui viewport whitelist; SDL_VIDEODRIVER=wayland still overrides.
            (4) Local Windows cross-build from Linux via Docker (QEMU's
            fedora-win64-cross image + mingw64-xz; see
            docs/cross-platform-builds.md) with a portable dist/ folder and a
            repo-root start.bat; requires --disable-sdl --disable-sdl-image
            --disable-gtk (SDL2.dll aborted process startup on Windows).
            (5) macOS: fixed epoxy wrap wrongly enabling CONFIG_OPENGL/EGL
            code (fatal missing egl_generated.h), completed SDL3's framework
            list (Metal, QuartzCore, IOKit, Cocoa, CoreVideo). Verified live
            on all three platforms (Linux Vulkan+PipeWire, Windows-VM
            software-render+WASAPI, macOS Metal+CoreAudio).

2026-07-23  FIX: retro-go menu flicker + black-screen/BSOD on quit-to-menu, all
            rooted in the async JPEG/LTDC work. (1) JPEG model now models input
            backpressure: IFTF/IFNFF clear when a decode job is posted and DIR
            writes are ignored while it runs -- the HAL feed loop (which passes
            its full buffer SIZE as InDataLength and relies on EOC to stop)
            could pump its source pointer off the end of AXISRAM (BusFault ->
            retro-go BSOD) and stray FF D9 bytes in over-pumped garbage posted
            bogus decode jobs that trampled real results (black covers, stuck
            first decode -> black screen on return to menu). (2) CONFR0.START
            bumps the job epoch so a stale in-flight decode can't publish into
            a new operation (wrong cover flashing). (3) LTDC captures are now
            snapshotted at the reload instant even when the compositor worker
            is busy (staged_job slot, promoted when the worker frees) -- the
            old defer-and-reread-next-vblank path captured retro-go's menu
            mid-redraw (it draws into the displayed buffer on alternating
            frames), the confirmed cause of the periodic carousel flicker.
            Plus a draw-quiescence debounce for the non-VBR fallback, and
            GNW_AUTO_INPUT now takes an optional hold duration (t:btn:secs,
            for boot-time bank-select combos). Confirmed by ear/eye by the
            project owner; the remaining "Celeste over SMW" cover overlap was
            verified on real hardware to be retro-go's own layout quirk
            (mixed-width covers), i.e. faithful emulation, not a bug here.

# Changelog

## 2026-07-23 — FEATURE: multi-context Settings window and RTC host sync
- Developed a dual-context native OS window architecture for the Settings 
  menu (`m_settings_window`), bypassing ImGui's native viewport limitations on
  Wayland. The second window operates independently with a decoupled ImGuiContext, 
  its own high-DPI scaling variables (`g_last_scale_settings`), and explicit font
  atlas rebuilds to ensure perfectly scaled ui elements across displays.
- Patched an active OS cursor flicker issue by preventing the primary game context
  from enforcing its 3-second cursor idle-hide rule whenever the Settings window
  is active.
- Added a "Sync RTC to Host Time" toggle to the System Settings tab, exposed as
  the `sync-host` QOM property on the `gnw-h7b0-rtc` device. Bound it to the UI 
  via `gnw_h7b0_rtc_set_sync_host()` so the emulated RTC correctly tracks the
  host time on boot and toggle.

## 2026-07-22 — FIX: stock-firmware "crunchy audio" root-caused and fixed (LTDC vblank re-phasing)

- Root cause of the long-open stock Mario audio-corruption bug (see the
  "what is ruled out" entry below): `gnw_h7b0_ltdc.c` re-anchored the
  vblank timer at "now + frame_ns" from `recalc_timers()`, which the
  SRCR write handler calls on **every** per-frame VBR reload request.
  Since firmware writes VBR ~1.4ms after taking the vblank IRQ, every
  frame became (guest work + one full period) ≈ 18.1ms — a rock-steady
  55.15fps instead of 60.05. Stock's NES-emulator audio producer is
  paced by this vsync chain while its SAI DMA drains at a metronomic
  48kHz, so the guest structurally under-produced by ~8%: measured ~20%
  of DMA half-buffers handed off as ring-underrun **silence** (zero-
  filled, 2-chunk bursts ~13x/s) — the audible crunch. Not a synthesis,
  rate-decode, or DMA-model bug: the guest's own bytes contained the
  gaps (verified by tapping the DMA buffer and listening offline).
- Fix: `recalc_timers()` is now phase-preserving (keeps the free-running
  vblank deadline grid unless it is stale/unarmed/wedged), the per-tick
  re-arm advances on the deadline grid instead of dispatch time (same
  discipline as `gnw_h7b0_dma_schedule_next()`), and the frame period is
  computed as an exact fractional muldiv64 period rather than a
  truncated integer Hz. Verified: 60.07fps, silent chunks 20%→0 (all
  remaining zero-runs are legitimate musical rests), confirmed clean by
  ear.
- Investigation tooling that made this findable, all env-gated and inert
  by default: `GNW_AUTO_INPUT` scripted button presses
  (`gnw_h7b0_gpio.c`; Mario has no START button — use A),
  `GNW_AUDIO_TAP=<file>` raw dump of the exact guest DMA audio bytes
  (`gnw_h7b0_sai1.c`), and `GNW_AUDIO_TRACE=1` stderr traces of DMA
  notifies / TIM5 CNT reads / fifo drops / VBR writes.
- Ruled out along the way (measured, for the record): TIM5 vs SAI rate
  mismatch (matched to 0.006%), a +0.5% producer-clock bias A/B (no
  change), integer-truncated 60Hz vblank alone (0.08%, too small), fifo
  drop path (zero drops), DMA2D latency (model is synchronous), DWT
  CYCCNT (stock never touches DWT).

## 2026-07-22 — fix: TIM2-TIM7 kernel clock was 4x too fast (APB1 prescaler ignored)

- `hw/misc/gnw_h7b0_rcc.c` gains `gnw_h7b0_rcc_get_timer_ker_hz()`, and
  `hw/misc/gnw_h7b0_tim2.c` now uses it for both the update period and the
  `CNT` tick rate, instead of using `gnw_h7b0_rcc_get_hclk_hz()` directly.
- STM32H7 timers do not run at PCLK: with `CFGR.TIMPRE=0` the kernel clock
  is PCLK1 for an APB1 prescaler of /1 and **2 x PCLK1** for anything
  larger (4 x above /2 when TIMPRE=1). The old code hardcoded
  `timer_clk == HCLK`, which is correct *only* for retro-go's config
  (APB1 /2, where the x2 rule exactly cancels the /2) — and that is
  precisely why retro-go never showed a symptom.
- Stock Nintendo firmware runs **APB1 /8 with TIMPRE=0** (`CDCFGR2 =
  0x00000660`, `CFGR = 0x0000001b`, read off the physical device), so its
  real timer clock is `2 x HCLK/8 = HCLK/4`.
- Verified against real hardware rather than derived: TIM5's `CNT`
  advances at **1.003MHz** on a physical H7B0 running stock Mario
  (`PSC=0x15`, i.e. a 22MHz kernel clock — the intended 1us time base).
  This model produced **3.998MHz**, exactly 4x fast; after the fix,
  **0.997MHz** (-0.5%, the residue being HCLK estimation).
- Note for future timing work: this did **not** change stock's audio
  corruption, despite that firmware polling this exact counter 540x/sec.
  Getting it right was still necessary, and it eliminates the time base
  as a suspect (see the audio entry below).

## 2026-07-22 — fix: register write masks silently dropped writable bits

Auto-generated `*_WMASK` tables (`include/hw/misc/gnw_h7b0_regs_*.h`)
narrower than the hardware's real writable field set, so firmware writes
were silently discarded and read back wrong. Found by diffing our register
state against a physical device running identical firmware, then audited
across every generated header against the vendor CMSIS definitions in
`sdk/cmsis-device-h7/Include/stm32h7b0xx.h`.

Fixed:

| register | was | now | dropped |
|---|---|---|---|
| `SAI1 AFRCR` / `BFRCR` | `0x00067fff` | `0x00077fff` | bit 16 `FSDEF` |
| `DMA1 S0CR` | `0x01efffff` | `0x01ffffff` | bit 20 `TRBUFF` |
| `OCTOSPI2 WCCR` | `0x2f3f3f0f` | `0xaf3f3f3f` | bit 31 `SIOO`, bits 5:4 `ISIZE` |
| `OCTOSPI2 WPCCR` | `0x2f3f3f3f` | `0xaf3f3f3f` | bit 31 `SIOO` |

`AFRCR` is hardware-confirmed: the device reads `0x00051f3f` where this
model read `0x00041f3f` for the same firmware write; it now reads
`0x00051f3f` too. The other three are provable from internal contradiction
without any hardware — `S0CR` was narrower than the seven identical
sibling streams in its own header, and `WCCR`/`WPCCR` were narrower than
the identically-laid-out `CCR` beside them.

Audited and deliberately NOT changed (real gaps, but no firmware this
project boots writes those bits — recorded so the audit isn't repeated):
`OCTOSPI2 DCR4` REFRESH modelled 16-bit not 32; `DAC1 CR` TSEL top bit;
`WWDG CFR` `WDGTB_2`; `SYSCFG CCCR`/`CCCSR`; `TAMP ATCR1` `ATOSEL4`;
`LTDC SSCR` HSW modelled 10-bit not 12 (the panel's HSW is far below the
10-bit limit); `SAI xCR1` bit 27 `MCKEN` (stock leaves it clear, and our
`ACR1` matches the device exactly). RCC, TIM2/TIM1, PWR, CRS, FMC, RTC,
GPIO, EXTI, CRC, IWDG, DBGMCU, JPEG, SPI, FLASH and OCTOSPIM audited
clean. `regs_adc.h` has gaps but is dead code — `gnw_h7b0_adc.c` does not
include it.

## 2026-07-22 — TIM2-TIM7: CNT is now a real, live counter

- `hw/misc/gnw_h7b0_tim2.c`: `CNT` reads were a plain register shadow,
  returning whatever the guest last wrote. Stock Mario configures TIM5 as
  a free-running 32-bit microsecond time base (`CR1=1, PSC=0x15,
  ARR=0xffffffff`) and polls its `CNT` **540 times a second** — measured,
  and it is the only timer register stock reads at all. Every one of
  those reads answered "no time has passed".
- `CNT` now extrapolates from a latched reference: `cnt_base +
  elapsed_ns * tick_hz / 1e9`, wrapped at `ARR+1`, against the live HCLK
  and prescaler. The reference re-latches on anything that invalidates
  it (`CNT`/`PSC`/`ARR` writes, `CEN` start/stop, `EGR.UG`, reset), and
  travels in vmstate (bumped to v2) so snapshots don't make the counter
  jump.
- Found while chasing stock-firmware audio corruption. It is a real bug
  independent of that, but **did not fix the audio** — the measured
  symptom (see below) was unchanged, so it is recorded here as its own
  fix, not as the audio fix.

## 2026-07-22 — stock-firmware audio corruption: what is ruled out

Not fixed. Recording the evidence so the next attempt doesn't re-tread
it. Symptom: stock Mario audio sounds "crunchy/thuddy", like something is
"echoing" and "playing more than it should". Retro-go audio is unaffected.

**What the audio actually is**, because it frames everything: the stock
Game & Watch Mario firmware runs the real NES Super Mario Bros through its
own embedded NES emulator. So the guest is emulating an **NES APU** —
square/triangle/noise channels — and then feeding the result to SAI1 via
DMA. The same music sounds crisp in a desktop NES emulator. Whatever is
wrong is happening to (or inside) that guest-side synthesis, not to a
sampled audio stream.

Recorded captures are preserved in `backup/audio-investigation/` (that
path is gitignored; see the README there). They include 0.65s of PCM read
off the **physical device**, which needs hardware and a debug probe to
reproduce.

Measured, on the exact byte stream handed to the audio backend:

- Audio is clean *inside* each 120-sample DMA half-buffer and breaks at
  the joins: sample-to-sample discontinuity at half-buffer seams is
  **1.70x** the mid-chunk value, and **90%** of all large discontinuities
  land exactly on a seam.
- Chunk ordering is already optimal: A,B,A,B scores 308 at the joins vs
  1734 for B,A and ~2230 for either half alone (interior ideal: 181).
- No duplicate emission: zero stale (byte-identical) halves, zero
  repeated source addresses, zero FIFO drops; exact 96000 B/s, and
  half/full notifications alternate perfectly at 200+200 per second.
- The dumped stream sounds wrong when played directly as a WAV outside
  QEMU, so the guest genuinely generates this audio — the SAI path
  reproduces it faithfully.

Also measured against a **physical H7B0 running the same stock Mario
firmware** (via `gnwmanager gdbserver` + OpenOCD `monitor mdw`, which
reads without halting): every clock and SAI register matches this model
exactly — `PLLCKSELR`, `PLLCFGR`, `PLL2DIVR` (`0x01013017`), `PLL2FRACR`,
`CDCCIP1R` (SAI1SEL=PLL2), `ACR1` (`0x004b1280`), `ACR2`, `ASLOTR`. So the
48kHz rate and the whole clock-decode path are confirmed correct, not
merely plausible.

Ruled out with evidence: stereo/mono misconfiguration (stock sets
`MONO=1`, and lag-1 < lag-2 correlation confirms true mono); DMA snoop
duplication (`ndtr` is the configured count and never varies); firmware
polling `NDTR` (zero reads); sample-rate/resampler error (kernel clock is
12.289MHz ~= the canonical 12.288MHz for 48kHz, confirmed identical on
hardware, stable, never reprogrammed after voice open); DMA double-buffer
mode (modelled correctly, and stock does not enable it, `CR=0x00062d57`
with `DBM=0`); frozen `TIM5.CNT` (fixed above, symptom unchanged); the
timer kernel clock being 4x fast (fixed above and verified against
hardware to 0.5%, symptom unchanged).

Two more dead ends worth not repeating:

- **The corruption is not a reinterpretation of the captured bytes.**
  Eleven transforms of the captured stream were generated and listened to
  (playback at 24k/22.05k, decimate-by-2, even/odd de-interleave, treat as
  stereo pairs, byte-swap, crossfade the seams away, drop alternate
  chunks). The untouched baseline was closest to what the emulator plays;
  every transform was worse.
- **`-icount shift=2` made it worse**, not better — slower and with the
  artifacts accentuated. Pacing the guest to a fixed instruction rate is
  not the answer.

Claims made during this investigation that did **not** survive, retracted
here so they are not re-derived: "real hardware holds sample values while
ours drifts by ±1" came from a single 5ms device snapshot and collapsed
once 12 live snapshots were taken (held-sample fraction is 74.4% on
hardware vs 74.5% here — effectively identical). Related step-size
comparisons were invalid because the device's output is ~14x quieter
(its volume setting), so the two were never on the same scale.

Still unexplained and worth picking up next:

- Our output is **duller** than the device's: 2.36% of energy above 5kHz
  vs 4.78%, spectral centroid 358Hz vs 418Hz (different musical passages,
  so suggestive rather than conclusive).
- Our audio carries strong broadband **amplitude modulation** that a
  known-good reference recording does not — modulation energy at 56Hz,
  75Hz and 371Hz at 0.3-0.34 relative, where the reference has
  essentially nothing above ~20Hz. Roughness of exactly this kind is what
  "crunchy" sounds like.
- Our DMA half/full interrupt cadence **jitters**: mean 2499.8us against
  an ideal 2500us, but sd 0.2% with periodic excursions to 1414us/3586us
  (43% early/late). Real DMA hardware is metronomic. This is the leading
  remaining suspect.

Technique note for whoever resumes this: `sendkey` via the QEMU monitor
does not reach this guest, so audio captures need a human playing. Dump
the buffer in `gnw_h7b0_sai1_dma_notify()` right after
`cpu_physical_memory_read()` and analyse offline. On the device side,
breakpointing the DMA2_Stream6 ISR (vector at `0x08000154`) and dumping
`M0AR` per hit yields consecutive blocks, but stretches wall-clock time
~10x between blocks, which visibly garbles what the firmware synthesises
— itself evidence that stock's audio generation is time-based.

## 2026-07-22 — fix: TIM2 ARR overflow flooded the main loop (the perf bug)

- `hw/misc/gnw_h7b0_tim2.c`: `gnw_h7b0_tim2_period_ns()` computed
  `(uint64_t)(arr + 1)`, evaluating `arr + 1` in 32-bit first. For
  `ARR = 0xffffffff` — both the reset value and how firmware sets up a
  free-running 32-bit microsecond time base — that wraps to 0, so the
  period collapsed to 0, hit the 1us MIN clamp, and a ~277-second update
  event was modelled as a 1MHz one. The timer re-armed itself as fast as
  the main loop could dispatch it: **130,000-180,000 virtual-clock
  expiries per second**, measured, on stock Mario (TIM5: CR1=1, PSC=0x15,
  ARR=0xffffffff). Widened before incrementing, and switched to
  `muldiv64()` since `(2^32) * 22 * 1e9` overflows `uint64_t` too.
- This was the long-standing "emulator feels slow" bug, and it was never
  about CPU emulation cost. Stock Mario, measured before/after:

  |                     | before | after |
  |---------------------|--------|-------|
  | main loop thread    | 86.8%  | 2.5%  |
  | guest vCPU (TCG)    | 4.4%   | 7.5%  |
  | whole process       | ~100%  | ~15%  |

  The profile that found it was dominated by `g_source_ref`,
  `g_mutex_lock/unlock`, `qemu_lockcnt_cmpxchg_or_wait` and `aio_bh_poll`
  — glib main-loop churn — while the guest sat at 1.5% busy. Any firmware
  using a free-running 32-bit timer (stock, retro-go) tripped it; gnw-doom
  does not, which is why it always "ran like a dream" by comparison.

## 2026-07-22 — fix: `-device loader,file=` boots came up halted

- `ui/gwemu.c`: `inject_default_args()` appends `-S` when it detects no
  firmware image, so the GUI can come up with nothing loaded (`--gui`).
  Its detection only recognized `-global gnw-h7b0-soc.*-image=`, so every
  `-device loader,file=...` invocation — `boot_qemu.sh --ephemeral`,
  `boot_qemu.sh --diag`, and hand-written command lines following them —
  was classified as imageless and silently had `-S` appended. Those boots
  sat in `paused (prelaunch)` with the vCPU thread accumulating zero
  ticks: a black window that had to be un-paused by hand from the GUI,
  with nothing on stderr explaining why. A generic-loader device with a
  backing `file=` now counts as a real image.
- Worth knowing for anything performance-related: this invalidates
  informal speed comparisons made against `--ephemeral`/`--diag` boots.
  Attaching a debugger to such a run resumes it (a gdbstub attach calls
  `vm_start()`), so a VM could look like it was running normally while
  actually having been started by the measurement itself.

## 2026-07-22 — USART1 transmit model

- `hw/misc/gnw_h7b0_usart1.c`: minimal transmit-only USART1, promoted out
  of `create_unimplemented_device()`. ISR always reports the transmitter-
  ready/idle flags; TDR writes go to a chardev (bound to serial port 0, so
  `-serial stdio`/`-serial file:` gives guest printf output). RX is not
  modeled. Trigger was the retro-go porting toolkit's test firmware (used
  by the gnw-doom homebrew port), which makes USART1 its printf console and
  hung forever at `while (!(USART1->ISR & TXE));` against the log-only stub.
  With this, that firmware + gnw-doom boot to a rendering, running game.

## 2026-07-19/20 — repo cleanup, CI/release pipeline, GUI foundation, C-ported asset tooling

- Repo cleanup pass: deleted 17 dated `docs/session-*.md` investigation
  diaries (~6900 lines, findings already live as code comments), rewrote
  `docs/STATUS.md`/`CLAUDE.md`'s doc map as real snapshots instead of
  growing narratives, squashed sibling-repo path references throughout,
  audited every script for hardcoded-path portability, un-vendored
  `STM32H7B0.svd` (5.2MB, now user-supplied like `rm0455.pdf`/`sdk/`
  instead of committed), added `docs/peripheral-coverage.md` (hand-
  maintained real-model/stub/unmodeled index, replacing a mechanically-
  regeneratable per-register audit dir that had zero curated content).
- Added `.github/workflows/build-check.yml` (Linux, every push/PR) and
  `release.yml` (Linux/Mac-arm64/Mac-x86_64/Windows, tags + manual
  dispatch). Confirmed actually green via real test-tag CI runs (not just
  "should work") across several rounds of real, distinct failures: missing
  `tomli`/`diffutils`/`zip` build deps, a `pipefail` bashism under a
  container's default non-bash shell, macOS ad-hoc code-signing needing
  the unsigned->signed rename step, and two real portability bugs in our
  own device-model code — `hw/misc/gnw_h7b0_rtc.c` used libc `timegm()`
  (unavailable on MinGW, fixed via QEMU's own `mktimegm()`), and
  `hw/arm/gnw_h7b0_soc.c` called `memory_region_init_ram_from_file()`
  unconditionally (CONFIG_POSIX-only upstream, genuinely unavailable on
  Windows) — Windows now gets real persistent flash-image backing via a
  from-scratch `CreateFileMapping`/`MapViewOfFile` implementation
  (wrapped with `memory_region_init_ram_ptr()`, the same portable
  host-pointer pattern `gnw_h7b0_otfdec.c` already used) rather than a
  silent ephemeral-RAM fallback.
- `contrib/gnw-tools/`: added `gnw-make-boot-images` (C port of
  `make_boot_images.py`) and `gnw-make-cfw-images` (C port of
  `make_cfw_images.py`, including a full from-scratch C port of
  gnwmanager's Thumb-2 assembler, lz77 decompressor, LZMA1 compressor
  (system `liblzma` — a vendored-LZMA-SDK-encoder attempt was tried and
  rejected first: it's a genuinely different codebase from `liblzma` that
  doesn't agree bit-for-bit on real data despite matching nominal
  parameters), AES-128, pre-extracted ELF symbol tables, and the
  `Firmware`/`Device` relocation engine). Both byte-exact verified against
  their Python originals for both games — this is groundwork for the GUI
  to call as real linked library functions instead of shelling out.
- GUI Phase 1 (`58fd37cdcb`): ported xemu's SDL3+ImGui HUD foundation with
  zero Xbox content — Xbox-domain files (disc/XBE UI, NV2A debug panels,
  Xbox controller binding, snapshot-manager's Xbox-save assumptions) were
  never copied in, not stripped after the fact; the safe, generic plumbing
  (viewport/font/animation/scene management, widget helpers) was ported
  close to as-is. Verified with real evidence (screenshot of actual LTDC
  output with the new menu bar overlaid, process stable across checks),
  not just a clean build.
- Two real device-model perf fixes from the sibling `stm32h7b0-diag`
  suite's reports (`87ba4be1d9`, `31fc4b479b`): memory-to-memory DMA
  transfers were incorrectly falling through to a 48kHz-audio-pacing
  timer built for SAI1 (~395x latency gap, fixed by reading `CR.DIR`
  directly and treating M2M as low-latency like HASH already does); MDMA
  did a per-word `cpu_physical_memory_read/write` loop for bulk transfers
  (~21x gap from per-call dispatch overhead, fixed with a bulk-copy fast
  path for the contiguous case). A third finding (LTDC's full-framebuffer
  snapshot firing on any `SRCR.IMR` reload regardless of which layer
  actually changed) was fixed too (`905376c344`), narrowly scoped (pure
  register-equality check, no timing heuristics) given this file's history
  of real flicker/stutter regressions from broader changes.

- Added real minimal IWDG/LPUART1 device models replacing bare
  `create_unimplemented_device()` stubs, fixing wrong reset values
  (`IWDG_RLR`/`IWDG_WINR` real value `0x00000FFF`, `LPUART1_ISR` real
  value `0x000000C0` — both previously read `0x00000000`) found by the
  sibling `stm32h7b0-diag` suite's real-hardware comparison
  (`347ca514dc`). Also fixed two latent bugs in `scripts/gen_stub.py`
  itself this exposed (outdated `hw/sysbus.h` include path, outdated
  `class_init` signature — both predating this fork's v11.0.2 pin).
- Replaced `hw/misc/gnw_h7b0_crc.c`'s bit-serial (32-iteration-per-word)
  CRC-32 computation with the standard table-driven byte-at-a-time
  algorithm, fixing a ~11x QEMU-vs-hardware wall-clock slowdown the
  diag suite's `crypto_crc32` benchmark found. Verified bit-identical
  across 64,000 random trials (`1d6506baea`).
- Removed an unconditional, un-rate-limited `fprintf()` on *every
  single* DWT register access in `hw/misc/gnw_h7b0_dwt.c` (committed
  since `d6a56195c7`, never noticed) — `DWT_CYCCNT` is the standard ARM
  cycle counter real firmware uses for precise timing measurement, so
  this was a real, previously-unnoticed source of wall-clock inflation
  for exactly the kind of timing-sensitive benchmarks the diag suite
  has been flagging as broadly slower in QEMU (`93d54eb378`). Also
  cleaned up two lower-severity one-shot (not per-access) debug prints
  in `hw/arm/armv7m.c` found in the same sweep; confirmed via a full
  audit that no other `gnw_h7b0_*` device model has an unconditional
  per-access print (the one exception, `gnw_h7b0_gpio.c`'s
  `[gpio-debug]`, is gated on human button-press events, not a hot
  path, and was left alone).
- Investigated (not fixed) `hash_sha256`'s reported ~400x QEMU slowdown:
  confirmed `hw/misc/gnw_h7b0_hash.c` is already properly incremental
  (buffers into a message array, calls `qcrypto_hash_bytes()` once at
  finalize, not a naive per-word recompute) and the crypto backend
  itself is fast in isolation (~2ms for the exact benchmark workload on
  this host) — the remaining gap is most likely generic QEMU MMIO/TCG
  dispatch overhead across many register accesses (compounded by the
  DWT bug above, now fixed) rather than a HASH-specific bug. Not fully
  root-caused; live in-QEMU profiling was attempted but blocked by
  environment process-management flakiness.

## 2026-07-14 — fixed DMA2D R2M and CRC_POL bugs; narrowed LTDC idle-fallback trigger

- Fixed two real device-model bugs reported by the sibling
  `stm32h7b0-diag` correctness-test suite (real-hardware comparison,
  both confirmed against RM0455): DMA2D's R2M fill mode was
  double-converting `OCOLR` (real firmware already pre-packs it into
  the output format, so the emulator's own ARGB8888-assuming encoder
  was converting it a second time); `CRC_POL`'s reset value was
  `0x00000000` instead of the real `0x04C11DB7`, inherited from an
  incorrect `STM32H7B0.svd` reset-value entry (`7f5c5afbe0`).
- Narrowed the LTDC non-VBR idle-fallback (`srcr_idle_ticks`, from the
  GBC-menu black-screen fix) to also require evidence of a genuine
  structural layer-config change (pixel format/layer-enable/window
  geometry, excluding CFBAR which flips every frame during normal
  double buffering) before overriding a still-true `vbr_active`
  (`87168aeef3`) — confirmed live that ordinary in-game frame-skipping
  (the same `frame_integrator` mechanism behind the general stutter)
  also suppresses VBR reloads for the same duration as a genuine
  abandoned transition, so the idle timer alone fired just as often
  during normal stutter as during the real bug, risking mid-draw
  tearing/flicker. **This fix has not yet been re-confirmed against the
  actual reported menu-flicker regression** — it was developed while
  chasing a different, unconfirmed lead (zelda3's attract loop) and still needs
  verification against the real repro.

## 2026-07-14 — fixed two RAM dirty-bitmap bugs found while re-profiling the frame_integrator stutter

- LTDC dirty-bitmap teardown (added alongside `3b946997d8`, meant to
  disable `DIRTY_MEMORY_VGA` logging on the framebuffer once normal
  VBR-paced gameplay resumes) never actually worked: it called
  `framebuffer_update_memory_section(..., 0, 0, 0)`, which internally
  does a zero-size `memory_region_find(root, 0, 0)` whenever the old
  section was bound, silently re-enabling logging on whatever real RAM
  sits at guest address 0 instead of leaving nothing tracked. Fixed by
  tearing down directly (`memory_region_set_log(false)` + `unref` +
  clear, no redundant lookup).
- AXISRAM1/2/3 were declared as three separate `MemoryRegion` objects in
  `hw/arm/gnw_h7b0_soc.c` despite real H7B0 AXI SRAM being one genuinely
  contiguous ~1MB block (RM0455, confirmed zero gap between the three
  subdivisions). Any guest buffer straddling one of those artificial
  boundaries — including the actual framebuffer, which spans
  AXISRAM1/AXISRAM2 — was unreachable via a single `memory_region_find()`
  call, meaning the LTDC non-VBR fallback's RAM dirty-bitmap tracking
  (landed in `343fbc19ed`) had silently failed to bind on every single
  call since it landed, always conservatively defaulting to "assume
  dirty." Fixed by merging the three into one `MemoryRegion` spanning
  the real contiguous range — a more faithful hardware model, not just a
  workaround (`a03ba2854a`).
- Neither bug explains the pause-resume stutter's own CPU cost during
  the burst itself (profiled at >50% of cycles in generic
  `notdirty_write`/CPU-TLB-path machinery specifically during the
  burst) — `tb_flush` and `tlb_flush` call frequency were both directly
  measured and ruled out as the cause. Root mechanism still open; likely
  in `notdirty_write`'s/`physical_memory_is_clean()`'s own dirty-bitmap-
  client interaction, not yet traced to a conclusion.
- A detour investigating a suspected excessive-MPU-write/`tlb_flush`
  theory (Cortex-M `prbar_write`/`prlar_write` in `target/arm/helper.c`
  unconditionally call `tlb_flush()` on every MPU region-config write)
  was ruled out by direct measurement: only ~200 `tlb_flush` calls total
  across the whole session, none during the active burst window.

## 2026-07-14 — fixed GBC-core-to-menu black screen (vbr_active idle-fallback gap)

- Root cause confirmed live: the transition can end on a VBR-type
  `SRCR` reload (not IMR) with no further reloads ever coming, so
  `vbr_active` (only ever reset on IMR) latches true permanently and
  blocks the no-VBR auto-capture fallback forever, freezing on whatever
  frame was active at that last VBR reload.
- Fix: a new `srcr_idle_ticks` counter (reset on every `SRCR` write,
  incremented each vblank tick otherwise) lets the fallback also engage
  once VBR has gone idle for ~130ms, but only combined with the RAM-
  dirty check landed in `343fbc19ed` — the same combination that makes
  this safe where a bare elapsed-time idle guess previously wasn't (a
  stalled game isn't writing new framebuffer content during its stall,
  so it can't spuriously re-arm and reintroduce mid-draw tearing the way
  the earlier reverted timeout attempt did) (`3b946997d8`).
- A detour mid-investigation into a suspected DMA1-Stream0/SAI1-audio
  NVIC interrupt-storm hang was pursued and then retracted: real-time
  breakpoints at `HAL_DMA_IRQHandler`'s entry and return both hit
  cleanly in sequence, proving the handler runs to completion normally.
  The earlier "stuck forever" reading came from naive `halt()`-based PC
  sampling in this gdbstub, which appears to snap to interrupt-vector
  boundaries rather than the true instantaneous PC — not a real hang.
  Not a device-model bug; no code change from that detour.

## 2026-07-14 — landed all four perf-improvement candidates from the research plan

- DMA2D: replaced the YCbCr->RGB conversion's floating-point BT.601
  constants with Q16 fixed-point equivalents (verified bit-identical
  output across the full cb/cr range), and batched per-pixel
  `cpu_physical_memory_read`/`write` calls (and per-pixel CLUT lookups)
  into one read/write per row plus one CLUT load per transfer
  (`160e516579`).
- LTDC: added a `blend_over` fast path for fully-opaque/fully-transparent
  per-pixel-alpha pixels (measured 10.29%->2.40% CPU in one profiling
  scenario), and hoisted the per-pixel horizontal window-clip test
  (WHSTPOS/WHSPPOS, row-invariant) out of the row loop into a
  precomputed per-column array (`279b08904d`).
- LTDC: the non-VBR auto-capture fallback now skips a full frame
  recomposite when nothing that affects pixel output has changed, using
  QEMU's real RAM dirty-bitmap mechanism (`DIRTY_MEMORY_VGA`, the same
  approach `hw/display/vga.c` uses) over Layer1/Layer2's actual
  framebuffer ranges, not a register-write proxy — a first attempt using
  register writes only was rejected mid-review because it would have
  frozen a game still rendering behind a static overlay with no further
  register writes; the RAM-dirty version was live-verified to not affect
  the (separately tracked) GBC-menu black-screen bug below (`343fbc19ed`).
- Confirmed via release-build disassembly that `gnw_h7b0_ltdc_resolve_layer`/
  `gnw_h7b0_ltdc_blend_over` are already fully inlined by GCC at `-O3` —
  no code change needed for that candidate.
- Full research writeup: `docs/session-2026-07-14-perf-improvement-candidates.md`.

## 2026-07-14 — performance-improvement research + new GBC-core-to-menu black-screen bug found

- Dispatched a forked agent to research a code-verified plan for finding
  materially more raw TCG throughput. Full writeup:
  `docs/session-2026-07-14-perf-improvement-candidates.md`. Top finding:
  DMA2D's YCbCr->RGB conversion (`hw/display/gnw_h7b0_dma2d.c:166`) uses
  floating-point math per output pixel on the JPEG cover-art path — a
  fixed-point BT.601 conversion is the highest-confidence, lowest-risk
  win identified. No safe generic QEMU/TCG-level tuning knob was found
  (BQL/MMIO dispatch and `accel/tcg/` both checked and ruled out); `-icount`
  confirmed to cost ~27% more host CPU for no throughput gain (determinism
  feature, not a speed fix). No code changes made yet — plan only.
- Found (not yet fixed) a new black-screen repro: returning to retro-go's
  main menu specifically from the GBC core (Link's Awakening DX)
  intermittently freezes on black. Confirmed via live debug logging that
  this transition never issues an `LTDC_SRCR.IMR` write, so the
  `vbr_active` flag added in `7ac66634e5` never resets, permanently
  disabling the no-VBR auto-capture fallback for this specific
  transition — same symptom as that commit's bug, different trigger its
  fix didn't cover.

## 2026-07-14 — general post-pause stutter: definitive root cause confirmed via direct hardware-vs-QEMU comparison

- Full writeup: `docs/session-2026-07-14-frame-integrator-hw-vs-qemu-comparison.md`.
- Supersedes the 2026-07-13 part 6 SysTick/`-icount` hypothesis below with
  a precise, confirmed mechanism, found by directly comparing identical
  breakpoint-based traces on QEMU and real hardware side by side (no
  resets, attached to an already-paused live repro on both), at the
  user's explicit direction after pushing back on treating this as
  inherent/unfixable.
- Root cause: `game-and-watch-retro-go-sd`'s `Core/Src/porting/common.c`
  (`common_emu_frame_loop()`/`open_pause_menu()`, shared by every core)
  tracks a leaky integrator (`frame_integrator`) of how far behind real
  time the emulated core is, and runs the core 2x per iteration
  (`skip_frames=2`) to pay off a backlog. On real hardware that costs a
  negligible fraction of a real frame at native clock speed, so the
  integrator stays in a small, bounded, spike-free steady-state
  (confirmed live: -2500 to -5000 across 60 samples, zero spikes). Under
  QEMU/TCG, that same catch-up work is measurably slow, and its own
  execution cost inflates the *next* iteration's measured elapsed time —
  feeding back into the integrator and demanding more catch-up, a
  genuine positive feedback loop confirmed live on QEMU (real spikes:
  6445→8112→11279 across three consecutive samples) that's structurally
  impossible on real hardware but forms naturally under TCG.
- Not a QEMU device-model bug. Two candidate real fixes identified, not
  yet implemented: clamping `frame_integrator`'s growth in the firmware
  (lowest-risk, now explicitly in-scope per the user), or `-icount`
  (bigger, not yet confirmed against this specific mechanism in
  isolation).
- Tooling notes: this fork's gdbstub acks `Z1` (hardware breakpoint) set
  requests but they silently never trigger — use `Z0` (software
  breakpoint) instead. `OCDBackend["openocd"]().open()` in default
  `attach` mode does not reset the target, confirmed safe to attach to a
  live, already-running/paused real device mid-session.

## 2026-07-13 (later same day, part 6) — general stutter root cause found: SysTick tick-loss under TCG load; `-icount` scoped as the real fix

- Full writeup: `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`
  (continuation of part 5's investigation).
- Replaced `vbr_ever_used` (permanent latch, committed `014612c688`) with
  `vbr_active` in `hw/display/gnw_h7b0_ltdc.c`/`gnw_h7b0_ltdc.h`: the
  latch fixed the pause-overlay flicker but permanently blacked out
  retro-go's main menu after playing any game (the menu never writes
  `SRCR` again to re-arm the no-VBR auto-capture fallback). `vbr_active`
  resets on every `SRCR.IMR` write instead — real screen/config
  transitions apply their layer config via IMR — rather than a one-way
  latch or an idle-timeout (idle-timeout was tried and reverted: real
  in-game firmware can leave multi-hundred-ms gaps between VBR writes on
  its own, so any timeout short enough to un-stick the menu was also
  short enough to spuriously re-arm the fallback mid-game).
- Found and resolved a second, unrelated black-screen cause: this
  project's flash images are persistent by default, and repeated hard
  `kill`s of QEMU mid-session while games were running had corrupted the
  backing files — fresh copies of `backup/qemu-images/zelda-*.bin` fixed
  it immediately, no code involved.
- Root-caused the general gameplay stutter the user flagged as the real
  priority (previously suspected SMW-specific): PC-sampled both SMW
  (SNES engine) and Link's Awakening DX (gnuboy GBC core) mid-stutter;
  both showed their hottest code in their own independent
  audio-rendering function, confirming a shared cause rather than a
  per-game one. Ruled out (via live tracing) a dynamic DMAMUX-based
  DMA/SAI1 rebind mechanism added in `77e86eacdd` for stock-Zelda/OFW
  compatibility as a suspect. Found the actual mechanism: SysTick's
  pending-IRQ state is a single bit, not a counter, so under QEMU/TCG (where
  `SysTick_Handler` takes far longer in real wall-clock time than on real
  hardware) a tick that fires while the CPU is still busy with the
  previous one is silently lost, regardless of how correctly
  `hw/core/ptimer.c` schedules its own deadlines. This corrects a specific
  claim in `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part
  22 (which attributed the gap to a ptimer catch-up policy bug — see the
  correction note added there) while keeping that doc's empirical
  host-throughput-correlation finding intact.
- Decision, agreed with the user: fixing this for real needs `-icount`
  (deterministic instruction-scaled virtual time), not another
  device-model patch. Scoped as a separate, larger follow-up effort
  rather than folded into this session, since it's a global timing-model
  change needing re-validation of `gnw_h7b0_dma.c`'s accumulator-scheduled
  timers, LTDC's vblank period, and SAI1 audio pacing.

## 2026-07-13 (later same day, part 5) — retro-go pause-overlay flicker/menu black-screen fixes, SMW stutter root-caused (not fixed here)

- Full session writeup: `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`.
- Fixed a real tearing regression in `hw/display/gnw_h7b0_ltdc.c`'s
  no-VBR auto-capture fallback (`gnw_h7b0_ltdc_vblank_tick()`): it fired
  on any vblank with `content_dirty` false, unable to tell "firmware that
  never uses VBR" apart from "firmware using VBR that just landed
  between two writes" (the latter grabbed a mid-draw frame). First fix
  (committed `014612c688`): a `vbr_ever_used` latch gating the fallback
  off once any `SRCR.VBR` write happens.
- Found that latch then permanently blacks out retro-go's main menu
  after playing any game (the menu paints pixels directly without ever
  writing `SRCR` again to re-arm the fallback). Replaced the permanent
  latch with `vbr_active`, reset on every `SRCR.IMR` write (real
  screen/config transitions use IMR) instead of a `vbr_ever_used`
  one-way latch or an idle-timeout (idle-timeout was tried and reverted:
  real in-game firmware can leave multi-hundred-ms gaps between VBR
  writes on its own, so any timeout short enough to un-stick the menu
  was also short enough to spuriously re-arm the fallback mid-game).
- Root-caused (not fixed here — belongs in the `game-and-watch-retro-go-sd`
  sibling repo) a real stutter during SMW gameplay to
  `external/smw/src/common_rtl.c`'s `RtlSetUploadingApu()` forcing a
  synchronous 10,000-emulated-cycle APU catchup burst on every SPC
  sound-driver reupload — negligible on real hardware, hundreds of ms
  under QEMU's TCG interpretation. Whether this generalizes to a shared
  qemu-gnw-side cause behind other cores' stutter (vs. SMW-specific) is
  still open per the user's own correct pushback — not yet confirmed
  either way.
- A separate, unrelated black-screen cause (corrupted persistent
  `backup/qemu-images/zelda-*.bin` state from repeated hard `kill`s of
  QEMU mid-session) was found and resolved with fresh image copies — no
  code involved; a reminder that this project's flash images are
  persistent by default and abrupt kills mid-write are a real corruption
  risk.

## 2026-07-13 (later same day, part 3) — gnw-web-builder integration: real HASH/erase device-model bugs, gdbstub live memory access, persistent flash images

- Full session writeup: `docs/session-2026-07-13-web-builder-integration-fixes.md`.
- Added a real STM32H7B0 HASH peripheral device model
  (`hw/misc/gnw_h7b0_hash.c`, MD5/SHA-1/SHA-224/SHA-256 backed by QEMU's
  own `crypto/hash.h`), replacing `create_unimplemented_device("HASH",
  ...)`. gnwmanager's RAM stub calls `HAL_HASHEx_SHA256_Start(...,
  HAL_MAX_DELAY)` after every flash write to verify it; the stub always
  reading 0 meant the digest-complete flag never set, permanently
  wedging every internal-flash write.
- Fixed `hw/misc/gnw_h7b0_flash_r.c`: internal-flash erase
  (`FLASH_CR1`/`CR2`'s `SER`/`BER`+`START` bits) was a pure register stub
  with no connection to the actual flash memory — erasing finished
  instantly and silently changed nothing. Now wired to the real
  `flash_bank1`/`flash_bank2` memory (`gnw_h7b0_flash_r_set_banks()`,
  called from `gnw_h7b0_soc.c`), so a sector/bank erase actually
  `memset`s the real backing memory to `0xFF`.
- `gdbstub/gdbstub.c`: memory read/write (`m`/`M`) packets no longer halt
  the VM. Upstream's blanket "any byte while running halts the target"
  rule was a protocol default, not a real requirement —
  `cpu_memory_rw_debug()` is exactly as safe to call from a running VM as
  a halted one. Every other command (registers, continue/step,
  breakpoints, etc.) still halts first, unchanged.
- Added optional persistent flash-image backing: `gnw_h7b0_soc.c` gained
  `bank1-image`/`bank2-image`/`extflash-image` string properties
  (`-global gnw-h7b0-soc.<name>=<path>`) that back that region directly
  with the named file (`memory_region_init_ram_from_file`, `RAM_SHARED`)
  instead of anonymous RAM seeded once via `-device loader` — guest
  writes (flashing, erasing) now persist to the file live. This is now
  `scripts/boot_qemu.sh`'s default (`--ephemeral` opts back out to the
  old discard-on-exit behavior) since it matches how real hardware
  actually behaves.
- New tool: `scripts/gdb_tap.py`, a logging TCP proxy for QEMU's GDB RSP
  port, with `TCP_NODELAY`/re-armed `TCP_QUICKACK` (fixes a ~40ms Nagle/
  delayed-ACK stall per request) and single-active-client preemption
  (QEMU's gdbstub only serves one client; a stale browser-tab connection
  no longer starves out a one-shot `gnwmanager --qemu` invocation).
- Cross-repo fixes found via the above (not in this repo, noted for
  context): `gnw-web-builder`'s mailbox status-poll interval (10ms ->
  150ms, was starving the guest CPU against the gdbstub's old
  resume-debounce), `backend/src/server.ts`'s QEMU-bridge socket
  (`TCP_NODELAY`), `qemuTransport.ts` (memory ops no longer pre-emptively
  halt), and `gnwmanager/ocdbackend/gdb_backend.py`'s socket
  (`TCP_NODELAY`, ~12x speedup on `gnwmanager --qemu info`).

## 2026-07-13 (later same day, part 2) — Fixed real per-pixel MMIO overhead in LTDC Layer2 and DMA2D, cutting effective slowdown roughly in half

- Investigated Zelda CFW's own reported ~50% real-hardware speed
  slowdown (separate from the Mario CSI/PLL1 fix below). Found LTDC's
  Layer2 compositing path (`hw/display/gnw_h7b0_ltdc.c`) and DMA2D's
  `M2M_PFC`/`M2M_BLEND*` transfer modes (`hw/display/gnw_h7b0_dma2d.c`)
  both issued a full `cpu_physical_memory_read()`/`_write()` guest-memory-
  translation call *per pixel* instead of batching a row at a time (JPEG
  cover-art's YCbCr decode did 3 such calls per pixel; DMA2D's L8 CLUT
  lookup re-fetched the same 256-entry table from guest memory once per
  pixel too). `perf record` on the live QEMU process confirmed this
  address-translation machinery (`phys_page_find`/`flatview_*`/
  `address_space_translate_internal`) was ~30% of total process CPU time.
  Batched both to fetch/write one row per `cpu_physical_memory_read()`/
  `_write()` call (CLUT loaded once per DMA2D transfer instead of once per
  pixel); confirmed via `perf` that this overhead is now gone from the hot
  path. Measured effect via SysTick-fire-rate counting (Zelda CFW):
  ~415Hz -> ~643Hz (real hardware is 1000Hz) — real, substantial, but not
  a full fix; remaining gap is genuine QEMU TCG instruction-interpretation
  cost, not further addressable at the device-model level. Also removed a
  stray capped-but-still-leftover debug `fprintf` in
  `hw/misc/gnw_h7b0_dma.c`'s stream-tick handler. Full investigation in
  `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` parts 20-23.

## 2026-07-13 (later same day) — Fixed: RCC_CR never mirrored CSION into CSIRDY, blocking Mario CFW's PLL1 overclock

- User reported Mario CFW running at roughly half real-hardware speed;
  confirmed via direct real-hardware register comparison (`gnwmanager`'s
  `OpenOCDBackend`) that QEMU's CPU was permanently stuck on HSI (64MHz)
  while real hardware runs the identical CFW image on PLL1. Root cause:
  `hw/misc/gnw_h7b0_rcc.c`'s `RCC_CR` write handler mirrored every other
  oscillator's `*ON` bit into its `*RDY` bit (HSI, HSE, PLL1/2/3, and
  `RCC_CSR`'s LSI) but had no case for CSI at all —
  `RCC_CR_CSION`/`RCC_CR_CSIRDY` weren't even defined. Firmware's
  `SystemClock_Config()` requests CSI ON as part of the same
  `HAL_RCC_OscConfig()`-style call that also configures PLL1; with
  `CSIRDY` never reachable, the oscillator-config sequence never
  reliably completed, and PLL1 never locked. Added the missing bit
  definitions (`include/hw/misc/gnw_h7b0_rcc.h`) and mirror logic
  (`hw/misc/gnw_h7b0_rcc.c`), matching the existing pattern for every
  other oscillator. User-confirmed real effect: every clock register
  (`PLLCKSELR`, `PLL1DIVR`, `CDCFGR1`, `CR`, `CFGR`) now matches real
  hardware bit-for-bit after boot, where before QEMU was permanently
  stuck on HSI. **A residual ~2x-slow tick rate remains despite every
  register now matching** — narrowed to QEMU's own internal clock-
  propagation/timing code (not firmware- or register-visible), not yet
  found. See `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
  part 20 for the full investigation and next-session starting point.

## 2026-07-13 — Fixed: two real LTDC bugs (AL44 unimplemented + Layer2/Layer1 compositing order backwards), resolving GAME/PAUSE menu invisibility

- Root cause of the Mario (and likely Zelda) GAME/PAUSE submenu bug
  from 2026-07-12 part 15, fully resolved and user-confirmed live.
  Button input, the menu's internal state machine, and the render
  dispatch were all working correctly the whole time (extensively
  re-verified this session) — the actual gap was entirely in QEMU's
  LTDC compositor, `hw/display/gnw_h7b0_ltdc.c`, and turned out to be
  two separate bugs stacked on top of each other:
  1. LTDC pixel format `6` (AL44 — 4-bit alpha + 4-bit luminance, used
     for anti-aliased overlay text) had no case in
     `gnw_h7b0_ltdc_capture_rows()`'s Layer2 bpp switch, so it fell
     through to `l2_bpp = 0` and silently skipped Layer2 compositing
     entirely even though the layer was enabled and had real content.
     Fixed with real AL44 decoding: alpha nibble applied directly,
     luminance nibble indexes a 16-entry CLUT sub-palette at `n*17`
     (confirmed against `sdk/stm32h7xx-hal-driver`'s
     `HAL_LTDC_ConfigCLUT()` AL44 branch and live-observed `L2CLUTWR`
     write patterns — firmware only ever loads the 16 diagonal
     entries, never a flat 256-entry table, ruling out an earlier
     attempt that treated AL44 like L8).
  2. Layer compositing order was backwards: Layer2 was composited
     first/bottom, Layer1 second/top — but real STM32 LTDC hardware
     always shows Layer2 *above* Layer1. Layer1 (RGB565, no alpha
     channel, always fully opaque) was therefore completely hiding
     Layer2's overlay content even after AL44 decoding was fixed
     correctly (confirmed via pixel-level tracing that Layer2 was
     computing real, correct values the whole time). Fixed by
     compositing Layer1 onto the background first, Layer2 onto that
     result second.
  Added `LTDC_PF_AL44` to `include/hw/display/gnw_h7b0_ltdc.h`. See
  `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part 19 for
  the full investigation, including two real tooling lessons from this
  session (QEMU's gdbstub halts input-event delivery, not just the
  vCPU -- reading peripheral state while halted can show stale
  pre-input values; and a full, non-`-noanalysis` Ghidra pass is
  needed for reliable RAM-address xref tracing).

## 2026-07-12 (later session, part 15) — Mario GAME/PAUSE submenu bug investigated, not yet fixed

- User-reported: on Mario CFW, GAME/PAUSE do nothing from the clock
  face's main loop (POWER wake and TIME both work fine; GAME *does*
  correctly wake the device from its screensaver, confirming the raw
  input path is fine). Extensive investigation ruled out: the GPIO/
  EXTI/`read_buttons()` chain (confirmed correct), the GAME+LEFT
  retro-go-jump combo (correctly declines given our placeholder,
  content-free `mario-bank2.bin`), the per-frame dispatch gate at
  `FUN_0801056c` (`[ctx+0x770]`/`[ctx+0x772]` already satisfied on
  QEMU), and LTDC Layer 2 being undrawn (it's actively populated with
  real content on QEMU). Root cause not yet found — full findings and
  concrete next steps in session doc part 15. No code changes this
  part; also documented a recurring stale-GDBBackend-connection
  tooling gotcha that produced one false-negative capture this
  session.

## 2026-07-12 (later session, part 14) — real CRYP (AES-GCM) device model; Mario boots

- New device model `hw/misc/gnw_h7b0_cryp.c` + header: real AES-128/
  192/256 (own key schedule + cipher, no external crypto lib) and a
  spec-correct AES-GCM engine (GHASH, CTR keystream, INIT/HEADER/
  PAYLOAD/FINAL phase state machine) matching the real hardware
  register protocol, wired to CRYP_IRQn=79 at 0x48021000. Also
  implements ECB/CBC/CTR (ready for future use, not yet exercised by
  any traced boot path). Root cause: Mario's CFW boot performs a real
  AES-GCM decrypt (integrity-checked blob) from CRYP's own ISR and
  sleeps until it completes; CRYP was `create_unimplemented_device`
  (silent stub, never interrupts), so the ISR never ran and boot hung
  forever. User-confirmed: Mario now boots past this point.
- Found via the systematic stuck-PC -> caller -> divergence method
  (see session doc part 14) after a longer, less disciplined detour
  earlier in the session — noted there as a process lesson.

## 2026-07-12 (later session, part 14) — LTDC display frozen after dynamic framebuffer reconfiguration

- Fixed hw/display/gnw_h7b0_ltdc.c: dynamic LTDC framebuffer/format
  changes (e.g. retro-go's lcd_setup_framebuffers() RGB565↔LUT8 switch
  via HAL_LTDC_SetPixelFormat() + HAL_LTDC_Reload(VBR)) no longer leave
  the host display stuck on the last pre-change frame. Three
  interacting gaps: (1) SRCR.IMR reloads updated the active register
  set but never triggered a host capture (only VBR-write-time capture
  existed); (2) when a SRCR.VBR write was skipped because content_dirty
  was still true, the deferred vblank reload applied the new shadow
  registers but also never captured, permanently blocking further
  captures via the !content_dirty gate; (3) gnw_h7b0_ltdc_enabled()
  consulted shadow regs[] instead of the active set actually used for
  scanout. Fix: capture immediately after IMR reload, add
  vbr_deferred_capture to capture right after the vblank reload when
  the VBR-write capture was skipped, use active_* for enable checks,
  and clear content_dirty on gfx_update() early-return paths so the
  capture pipeline cannot wedge.

## 2026-07-12 (later session, part 13) — OFW input + audio + display transitions working in QEMU

- Buttons now raise real EXTI interrupts (SYSCFG EXTICR-muxed); TIME
  dual-wired PC5+PA2 (stock reads PA2/WKUP2 in default mode); keyboard
  map now configurable via `-global gnw-h7b0-gpio.keymap=...`.
- SAI1's DMA binding resolved at runtime from DMAMUX request 87 (was
  hardcoded to retro-go's DMA1 Stream0; stock uses DMA2 Stream6).
- DMA double-buffer mode (SxCR.DBM) modeled (CT toggle, EN stays set) —
  stock's audio engine streams via DBM; was killed after one buffer.
- DMA2D IRQ line now honors all six ISR flags (CTCIF etc.) — stock's
  palette-fade transitions sleep on CLUT-transfer-complete.
- SD SPI-mode fix (colleague report): ACMD41 now → transfer state once
  powered up; CMD17/CMD55 no longer rejected without a prior CSD read.
- SAI NODIV=1 rate decoding fixed (stock: PLL2P 12.288MHz / MCKDIV 4 /
  64-slot frames = 48kHz; was decoded as 12kHz through the NODIV=0
  formula, running audio AND all audio-paced firmware at 1/4 speed —
  also the real cause of "buttons don't react").
- Removed the vestigial mouse-button input mapping (clicks/wheel were
  silently pressing A/START/d-pad; a swallowed release latched START
  low forever, freezing OFW menu input — the "stops responding after
  the menu" bug).
- Result: interactive OFW fully working in QEMU — POWER wake, TIME
  transitions, correct-pitch audio, responsive input (user-confirmed).

## 2026-07-12 (later session, parts 11-12) — ZELDA BOOTS TO VISIBLE DISPLAY in QEMU

- The physical device's exact image pair (repo-root `zelda-patched.7z`:
  gnwmanager-CFW bank1 + patched 4MB extflash) is now what
  `boot_qemu.sh --patched` boots — bank1 verified byte-for-byte
  identical to live device flash over SWD. The pivot to the full CFW
  was an intentional project decision that had been lost across
  session summaries; QEMU had been booting a stock+2-byte-patch image
  with the wrong (stock, 64MB) extflash. All tracing scripts'
  entry-point source repointed to the patched bank1 (CFW replaces the
  reset vector).
- With the correct image pair plus this session's OSPI-auto-polling
  and OTFDEC fixes, **QEMU boots Zelda CFW to visible display output**
  (user-confirmed on screen; `LTDC_GCR.LTDCEN=1`, steady-state PC
  matches real hardware's). Remaining follow-ups (real OTFDEC AES-CTR
  for genuinely-stock encrypted extflash, true-stock boot's WFI wait,
  audio/input/gameplay verification) recorded in the session doc's
  part 12.

## 2026-07-12 (later session, parts 7-10) — RSTEN trap root-caused and fixed (OSPI auto-polling), OTFDEC device model added, two big methodology corrections

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
(parts 7 through 10).

- **Real fix (root cause of the part-6 self-trap)**: OSPI automatic
  status-polling (CR.FMODE==2) was completely unmodeled, so `SR.SMF`
  never set and stock Zelda's `HAL_OSPI_AutoPolling()` wait timed out
  into firmware's own `b .` error trap at `0x080164b8`. Implemented in
  `hw/misc/gnw_h7b0_ospi.c`/`.h` (`ospi_autopoll_evaluate()`: PSMKR/
  PSMAR match, AND/OR per CR.PMM, evaluated at command trigger).
- **Real fix (the next trap after that)**: new minimal OTFDEC device
  model (`hw/misc/gnw_h7b0_otfdec.c` + header, both instances wired at
  `0x5200b800`/`0x5200bc00`), implementing the key-CRC readback
  (`CONFIGR.KEYCRC`, exact `HAL_OTFDEC_KeyCRCComputation()` algorithm)
  that `HAL_OTFDEC_RegionSetKey()` verifies. Actual AES-CTR decryption
  of memory-mapped reads is NOT yet modeled — stock extflash is
  encrypted, so data (not control flow) read through OTFDEC regions is
  still wrong on QEMU; flagged as the known next gap.
- QEMU stock-Zelda boot now clears the whole OSPI/OTFDEC init sequence
  and parks in a legitimate `while (!flag) WFI;` wait (`0x0800e5a8`,
  flag `0x2000ad40`) for a not-yet-identified IRQ — no longer in any
  error path.
- **Methodology correction #1 (parts 7-9)**: the parts-6-8 "real
  hardware has code-like bytes at address 0, QEMU has zeroes" thread
  was a testing artifact — stale SRAM from a previous boot surviving
  SWD/`nSRST` resets (only a power cycle clears it), not a QEMU bug.
  Also documented: OpenOCD `wp` silently fails to register >4KB
  watchpoints on this board's `hla_target` (false-negative timeouts).
- **Methodology correction #2 (part 10)**: the physical device is
  running gnwmanager's FULL CFW patch set (reset vector replaced,
  OTFDEC disabled, save-crypto skips, ~100 flash diff ranges vs stock),
  not our 2-byte standby patch — real hardware is no longer a valid
  stock-behavior reference for OTFDEC/bootloader/save-crypto regions or
  anything above `0x1B3E0`. Check `gnw_patch/zelda.py`'s patch list
  before trusting any hardware trace in a given region.

## 2026-07-12 (later session, part 6) — real OCTOSPI IRQ modeled; firmware self-trap found, root cause still open

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
("Follow-up session (same day, part 6)" section).

- **Real fix**: `hw/misc/gnw_h7b0_ospi.c`/`.h` gained a real, level-
  sensitive IRQ line for OCTOSPI1/2 (`ospi_update_irq()`, gated by CR's
  `TEIE`/`TCIE`/`FTIE`/`SMIE`/`TOIE` against SR, re-evaluated on every
  SR/CR-affecting write) — neither instance had any IRQ modeled before
  this. Wired to NVIC in `hw/arm/gnw_h7b0_soc.c`
  (`OCTOSPI1_IRQn=92`/`OCTOSPI2_IRQn=150`, per
  `sdk/cmsis-device-h7/Include/stm32h7b0xx.h`), which also required
  bumping `armv7m`'s `num-irq` property `96`→`160` (NVIC sizes must be
  multiples of 32; 150 didn't fit in 96, tripped an assertion on boot).
- Found (not yet fixed): stock Zelda firmware deliberately traps itself
  in an infinite `b .` loop at `0x080164b8` on a HAL-style error
  return, tracing back to an OCTOSPI1 RSTEN-command helper
  (`FUN_0800e45c`/`FUN_080112b6`). The OCTOSPI IRQ fix above didn't
  resolve it — every core register matches between QEMU and real
  hardware at the exact failing checkpoint except one register's
  dereferenced value, which reads real code-like bytes at address
  `0x0` on real hardware but all-zeroes on QEMU. Root cause still
  open; leading theory is a firmware ITCM-populating copy loop that
  doesn't run identically (or at all) on QEMU. See the doc for the
  full trace and concrete next steps.
- A same-session attempt to fix the address-`0x0` divergence by
  aliasing flash bank 1 there was **wrong and reverted** — the SoC
  already has a legitimate ITCM region at that address
  (`ITCM_BASE_ADDRESS = 0x00000000`), and the new alias just shadowed
  it instead of fixing anything.
- New tooling: `scripts/probe_11536_state.py` (arbitrary-register +
  dereferenced-pointer + NVIC/peripheral-state comparison at a single
  checkpoint on both targets) and `scripts/resume_and_sample.py`
  (resume + repeated halt-and-sample-PC, to tell a genuine CPU stall
  apart from a breakpoint-detection artifact).

## 2026-07-12 (later session, part 5) — real ADC battery-threshold bug found and fixed via lockstep tracing

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
("Follow-up session (same day, part 5)" section, which also documents
the efficient checkpoint-bisection playbook used to find this).

- **Real fix**: `GNW_H7B0_ADC_FULL_BATTERY_RAW` (`include/hw/misc/gnw_h7b0_adc.h`)
  bumped `13500` -> `0xFFFF`. The old value only cleared one of stock
  Zelda's own 4 battery-level threshold tables (`FUN_0800320e` in a
  Ghidra decompile, thresholds up to ~41974) -- QEMU read battery level
  0 where real hardware reads a real nonzero level, silently skipping an
  entire boot-progress branch. Checkpoint-confirmed: `FUN_0800ec7a` now
  takes the same internal branch as real hardware, and the per-pass
  event-queue dispatch now fires identically on both targets, neither of
  which matched before this fix.
- Also fixed same day (part 4, folded in here since it's the same
  investigation thread): `hw/misc/gnw_h7b0_gpio.c` no longer forces
  GPIOC bit 8/13 or GPIOD bit 0 low at reset -- confirmed via direct
  real-hardware register reads that all read `0xFFFFFFFF`. This
  unblocked QEMU reaching the LTDC display-init entry point and a
  layer-window register write, matching real hardware at both, which it
  never did before.
- `scripts/boot_qemu.sh` gained `-audiodev pa,id=snd0` +
  `-global gnw-h7b0-sai1.audiodev=snd0` (QEMU had no audio backend
  configured at all) and a `--patched` flag to boot the
  standby-patched bank1 image for a fair comparison against patched real
  hardware.
- New `scripts/make_zelda_patched_bank1.py` (reproduces gnwmanager's
  standby-skip patch bytes locally) and `scripts/watch_state_byte.py`.
- Still open: real hardware's route to the LTDC HAL init (`0x08013704`)
  isn't the event-queue path just fixed -- a different, not-yet-found
  call to `FUN_0800eb90(3)` is the actual trigger. No display/audio on
  QEMU yet.

## 2026-07-12 (later session, part 2) — breakpoint-based lockstep tracing

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`.

- New `scripts/checkpoint.py`, `step_init_calls.py`, `watch_loop_flag.py`,
  `watch_write.py`, `lockstep_compare.py`: breakpoint-based (not
  single-step-based -- too slow over real hardware's SWD link)
  QEMU-vs-real-hardware live execution comparison at chosen checkpoints,
  cross-referenced against Ghidra decompilation.
- Confirmed QEMU tracks real hardware **bit-for-bit identically** from
  the real reset entry point through the constructor/init-array
  dispatcher, MPU/cache setup, ~20 subsystem inits, and into the main
  superloop's first 20 passes -- the previously-documented "counter
  never arms" blocker (`[r4+9]` exit flag, `[r4+0x60]` enable field both
  stay 0) is reproduced exactly on QEMU with this new scripted,
  repeatable checkpoint. Real-hardware confirmation at the same exact
  point is the next session's first task.
- Root-caused repeated real-hardware SWD disconnects
  ("OpenOCD lost contact ... CPU likely entered low-power/standby") to
  stock Zelda firmware's own state-6 standby handler triggering during
  the repeated resets this kind of tracing requires -- not a tooling
  bug. Decision: use `gnwmanager`'s patched-out-standby Zelda blob for
  future interactive real-hardware breakpoint-tracing sessions
  specifically (boot-behavior-accuracy work still uses the real stock
  dump). QEMU has no standby/low-power mode modeled yet -- flagged as
  necessary future work, not yet started.
- Found and fixed several real bugs in this session's *own tooling*
  (not firmware bugs): a missing breakpoint step-over before `continue`
  that made QEMU look completely stuck re-executing the same call
  forever (a pure tooling artifact -- see the doc's "Real, load-bearing
  bugs found and fixed in this session's own tooling" section for the
  full list, including a breakpoints-halt-before-not-after off-by-one,
  a Python socket timeout wedge, and stdout buffering hiding live
  progress).
- Reinforced: do not edit the `gnwmanager` package to add capabilities
  (reverted an earlier attempt this session) -- it has independent
  concurrent development happening outside this repo; write pure-
  consumer scripts against its existing public API instead.

## 2026-07-12 (later session) — register snapshot/diff tooling, 3 real reset-default fixes

Full narrative: `docs/session-2026-07-12-register-snapshot-diffing.md`.

- New `scripts/snapshot_registers.py` + `diff_snapshots.py` +
  `triage_diffs.py`: dump every SVD peripheral's register block from
  QEMU or real hardware (via `gnwmanager`'s backend abstraction,
  including its new `--qemu` gdbstub support) and diff/auto-classify
  against the SVD's own documented reset values.
- New `scripts/halt_at_entry.py`: deterministic reset-and-halt-at-the-
  real-entry-point via a real breakpoint (gdb-remote `Z1` for QEMU,
  OpenOCD `bp`/`wait_halt` for real hardware), replacing the racy plain
  `reset_and_halt()`/`reset halt` for cases needing true pre-firmware POR
  state. Built entirely on `gnwmanager`'s existing public backend API, no
  changes to the `gnwmanager` package.
- **Fixed 3 real reset-value bugs**, all the same class (a write-only
  "pulse" register whose write handler had no special case, so the
  generic mask-and-store path leaked the last-written value into
  readback instead of the real always-reads-0 behavior):
  `hw/misc/gnw_h7b0_gpio.c` (`GPIOx_BSRR`), `hw/misc/gnw_h7b0_rtc.c`
  (`RTC_WPR`), `hw/misc/gnw_h7b0_tim1.c` (`TIM1_EGR`).
- `hw/arm/gnw_h7b0_soc.c`: extended `create_unimplemented_device`
  coverage from 2 to 69 peripherals (every SVD peripheral not covered by
  a real device model), so a full-address-space register sweep (or any
  future gnwmanager/firmware probe) logs instead of BusFaulting.
- New `scripts/make_boot_images.py` + `boot_qemu.sh`: standardized,
  correctly-sized (0xFF-padded to real `FLASH_BANK_SIZE`/`EXTFLASH_SIZE`)
  bank1/bank2/extflash boot images and the one launch command going
  forward, replacing ad hoc partial-dump QEMU invocations. Deliberately
  omits `-d guest_errors,unimp`/other unbounded logging flags — that
  combination filled `/tmp` and destabilized the host multiple times
  this session.
- Confirmed (not a bug): RCC's apparent reset-default "anomalies"
  (mirrored `ENR`/`LPENR` register block at a constant `-0x60` offset,
  non-zero trim/backup-domain registers) are real hardware behavior —
  factory calibration trim loaded by hardware itself, backup-domain state
  that legitimately persists across a warm reset by design, and an
  undocumented address-decode aliasing quirk present even at a guaranteed
  pre-firmware halt. None are fixable or worth modeling.

## 2026-07-12 — stock firmware boot investigation, PA0/WKUP1 fix

Full narrative: `docs/session-2026-07-12-stock-firmware-boot-investigation.md`.

- **Fixed `hw/misc/gnw_h7b0_gpio.c`**: stopped forcing PA0/WKUP1 low at
  GPIO reset. It was previously forced low to work around an unrelated
  early-boot write-storm hang from before `RCC_RSR.SFTRSTF` was fixed.
  Real disassembly, cross-checked against `gnwmanager`'s
  `gnwmanager/cli/gnw_patch/{mario,zelda}.py` (real, SHA1-hash-verified
  stock-firmware patch offsets, not a theory), shows this pin gates a
  "state-6 standby" handler (both patch files literally label it that,
  as part of a "warm-boot power-off fix": Mario `0x08005EF4`, Zelda
  `0x0800EA8C`) that firmware expects to read high (button not held) to
  do its real display-init work. With `RCC_RSR.SFTRSTF` already fixed,
  releasing PA0 no longer triggers the old storm — confirmed real forward
  progress on both Mario and Zelda with zero live pokes (genuine
  `SPI2->TXDR` traffic matching the known LCD panel bring-up command
  sequence).
- New sibling decompilation projects: `~/Nerd/git/gnw-mario-decomp` and
  `~/Nerd/git/gnw-zelda-decomp`, same Ghidra script toolkit in both.
- Found but not yet fixed: both games' main superloop only exits a
  timeout-gated wait once a counter (Zelda: `FUN_0800edfc`, threshold 39)
  crosses a threshold, but the counter never starts because its enable
  field (`[r4+0x60]`) is never written by anything — confirmed via a live
  hardware watchpoint over ~12s of real execution. Next session should
  pick up from here.
- Workflow: confirmed QEMU's SD model only requires power-of-2 sizing for
  cards ≤2GiB; above that (the user's real ~7.4GiB `sdcard.img`) only
  512K alignment is required — no qcow2 overlay needed, plain
  `-drive if=sd,format=raw,file=sdcard.img` works directly.

## 2026-07-11 (part 4 — bump pinned base v9.2.4 -> v11.0.2)

- **Merged upstream `v11.0.2`** (previously pinned to `v9.2.4`), via
  `git merge` rather than rebase to preserve existing history. Nearly all
  conflicts were upstream refactors in files this fork never touches
  (target/arm, migration, etc.) and were taken wholesale from upstream;
  the only real conflicts were in the 4 files we'd actually modified
  (`hw/arm/Kconfig`, `hw/arm/meson.build`, `hw/sd/ssi-sd.c`, `ui/sdl2.c`).
  Notably, upstream's SPI-mode response refactor in `hw/sd/sd.c`
  (idle-state bit now derived from real SD card state) superseded our
  earlier CMD58 idle-bit heuristic patch in `ssi-sd.c`, which was
  dropped in favor of the upstream fix.
- Ported all `gnw_h7b0_*` device models to v11.0.2's internal API churn:
  several `hw/*.h` headers moved under `hw/core/`, `exec/memory.h` ->
  `system/memory.h`, `ObjectClass.class_init`'s `data` param is now
  `const void *`, `DEFINE_PROP_END_OF_LIST()` sentinel removed from
  `Property` arrays, and the audio backend API was rewritten
  (`QEMUSoundCard`/`AUD_*` -> `AudioBackend *`/`audio_be_*` in
  `gnw_h7b0_sai1.c`). Also had to switch `gnw_h7b0.c`'s machine
  registration from `DEFINE_MACHINE()` to `DEFINE_MACHINE_ARM()` since
  `hw/arm/meson.build` moved most boards (including ours) into a new
  shared `arm_common_ss` source set that requires the ARM target-info
  interface to actually show up in `-M help`.
- Verified `qemu-system-arm` builds clean and boots
  `gw_retro_go_bank1.elf` normally under gdb (PC and `uwTick`
  progressing steadily across repeated samples, not just process-alive).

## 2026-07-11 (part 3 — VBR-gated capture timing, JPEG OFTF perf fix)

- **Gated LTDC framebuffer capture on firmware's `SRCR.VBR` write** instead
  of an independent fixed vblank timer (`hw/display/gnw_h7b0_ltdc.c`,
  developed in a separate concurrent session, reviewed and merged here).
  The old unconditional per-tick capture could sample the framebuffer while
  the emulated CPU was still mid-draw on a variable-cost frame (e.g. a
  JPEG-heavy coverflow redraw), a plausible root cause for the "flip back
  then snap" tearing symptom distinct from anything found earlier this
  session. Capture is gated on `!s->content_dirty` so an unconsumed capture
  isn't overwritten before the UI thread picks it up. **Not yet confirmed
  live whether this resolves the flicker** — known gap: content that never
  writes `SRCR.VBR` (single-buffered/IMR-only paths) no longer gets
  captured at all.
- **Fixed a real JPEG decode performance bug**: the model never set `OFTF`
  (output FIFO threshold), so firmware's polling loop (`JPEG_Process`) was
  always forced into the one-word-at-a-time `OFNEF` path instead of the
  real 8-words-per-check bulk path, multiplying the number of separate
  SR-flag-check MMIO round trips per decode by ~8x. `perf` showed this as
  the dominant cost (>16% of total CPU) during a menu scroll-loop
  workload; setting `OFTF` correctly (mirroring real hardware's FIFO
  watermark) cut that specific overhead to ~11% with zero change to
  decoded output (total DOR word-reads are identical either way — real
  hardware's own `JPEG_StoreOutputData` still reads one word at a time
  internally regardless of threshold, so this only reduces polling-loop
  *iteration* overhead, not real data volume). Ruled out GPIO/input
  reading as a contributor via the same profile — it doesn't appear in the
  hot path at all.

## 2026-07-11 (part 2 — real DMA2D/JPEG YCbCr blend pipeline, flicker still open)

Follow-on to the same-day LTDC/JPEG work below. Full writeup:
`docs/session-2026-07-11-dma2d-jpeg-ycbcr-pipeline.md` (and its predecessor
`docs/session-2026-07-11-ltdc-flicker-investigation.md`).

- **Fixed LTDC `RRIF` unconditional-assert bug** and **generalized LTDC's
  per-layer compositing** (color key, window-clip/default-color, generalized
  `BF1`/`BF2` blend formula, Bayer dithering, Layer2 CLUT/window-clip gaps) —
  both real, both tested, **neither fixed the reported coverflow flicker**.
- **Replaced the JPEG "hack buffer"** (a raw RGB565 QEMU-heap pointer handed
  directly to DMA2D, superseding this file's earlier 2026-07-11 JPEG entry
  below, which turned out to bypass `FGMAR`/`FGOR`/chroma-subsampling
  entirely) **with a real polled `DOR` register** that firmware's own
  `HAL_JPEG_Decode`/`JPEG_Process` polling loop drains into guest RAM,
  exactly like real polling-mode JPEG decode (no DMA involved) — decode
  stays fully synchronous, only the delivery mechanism changed.
- **Fixed `DMA2D_CR_MODE_MASK`** from 2 bits to the real 3-bit width —
  firmware's real cover-art blend mode (`M2M_BLEND_BG` = 5) was silently
  truncating to `M2M_PFC` (1), meaning the real blend firmware performs
  never actually happened in emulation before this.
- **Implemented real `BLEND_BG`/`BLEND_FG` "fixed color" semantics** (one
  side is a constant `FGCOLR`/`BGCOLR`, not a fetched buffer — confirmed via
  the real HAL header) and added a real `A8` input-format fetch. An initial
  pass that treated `BLEND_BG` like a two-buffer blend (fetching a nonexistent
  `BGMAR` buffer) was a real regression, caught via live testing, fixed same
  session.
- **Fixed a chroma-subsampling storage-size mismatch**: serving full-
  resolution (non-subsampled) Cb/Cr made our JPEG `DOR` output ~2x the size
  firmware's own destination buffer expects for real (subsampled) hardware
  output, so firmware's polling loop only partially drained it — visible as
  banded/static corruption on cover art. Fixed by subsampling Cb/Cr per the
  image's real hand-parsed SOF0 sampling factors.
- **Fixed an out-of-bounds row-wrap** in the new YCbCr fetch (reading past a
  decoded image's real width using the DMA2D transfer's larger configured
  geometry wrapped into the next row's bytes).
- Both corruption fixes are confirmed live-fixed by the user. **The original
  coverflow/menu flicker itself is still unresolved** — decode, compositing,
  and the final CPU blit into the LCD framebuffer are all now confirmed
  deterministic, ruling out this pipeline as the remaining cause. Next
  suspect: LTDC scanout/vblank timing relative to firmware's real swap
  cadence, not yet live-traced.

## 2026-07-11

- **Fixed the pause/options-overlay flicker (screen tearing)**: The root cause was QEMU's asynchronous UI refresh timer occasionally reading the active frontbuffer mid-draw. The guest uses DMA2D to draw overlays directly into the frontbuffer immediately after a `VBR` buffer flip. Fixed by introducing an internal `shadow_buffer` in the LTDC module that captures the fully composited guest frame at the very end of the 16ms window (the instant before the next `VBLANK`), decoupling QEMU's UI thread from the guest rendering entirely.
- **Implemented accurate LTDC frame pacing**: Replaced `QEMU_CLOCK_VIRTUAL` with `QEMU_CLOCK_HOST` for `vblank_timer` and `line_timer` to prevent `__WFI()` from fast-forwarding the virtual clock, ensuring a true 60 Hz real-time frame rate. Added exact scanline duration calculation via `TWCR` and `LIPCR` for true mid-frame `LIF` interrupt firing.
- **Added LTDC Layer 2 hardware blending support**: Replaced the single-layer hardcoding with a full dynamic pixel compositor in the LTDC capture pass, supporting `ARGB8888`, `RGB565`, `ARGB1555`, and `ARGB4444` blending using `L2CACR` constant alpha.
- **Implemented hardware JPEG decoding for cover art**: Replaced the dumb "instant done" stub in `hw/misc/gnw_h7b0_jpeg.c` with a real bitstream parser that accumulates writes to `DIR`. It intercepts `End of Image` markers (`FF D9`) and triggers a full decode using `stb_image.h`. To avoid the complexity of YCbCr 4:2:0 MCU block conversion, the decoded RGB buffer is bypassed directly into the DMA2D engine when a YCbCr foreground format (`FGPFCCR.CM == 0xB`) is requested, resulting in perfectly accurate cover art rendering in the `retro-go` UI.
## 2026-07-10 (part 3 — flicker/speed root-cause, DMA completion, battery)

Root-caused and fixed the flicker + "runs too fast" bugs flagged as the
prior session's starting point — they turned out to be two separate real
bugs, not one. See `docs/session-2026-07-10-part5-flicker-speed-dma-battery.md`
for the full narrative, including a still-open pause/options-overlay-specific
flicker that is confirmed *not* the same bug (ruled out GPIO input bounce,
LTDC enable/format toggling, and Layer2/CLUT).

- **Fixed the real flicker root cause**: `hw/display/gnw_h7b0_ltdc.c`'s
  `SRCR.VBR` bit was cleared the instant it was written, making
  `gnw-chainloader`'s/retro-go's `gui_refresh()`-style busy-wait on
  `SRCR & (VBR|IMR)` a no-op — that wait is real firmware's *actual*
  frame-rate throttle (confirmed by reading `gnw-chainloader`'s and
  `game-and-watch-retro-go-sd`'s real source, both checked out locally).
  Measured `gui_refresh()` looping at ~1000x/sec instead of ~60Hz as a
  result. Fixed: `VBR` now stays set until the real vblank tick, which
  applies the deferred `CFBAR`/`CFBLR`/`CFBLNR`/`PFCR` shadow→active
  reload *and* clears the bit, genuinely pacing the whole redraw loop.
- **Fixed the real "runs too fast" root cause**: `hw/misc/gnw_h7b0_dma.c`
  had zero transfer-completion logic and wasn't wired to the NVIC at all
  (16 DMA1/DMA2 stream IRQs, none connected). retro-go's audio playback
  paces emulated game speed off SAI1's DMA half/full-transfer-complete
  interrupt; with none modeled, it hung solid on starting a game. Fixed:
  real per-stream completion timing (derived from `NDTR` at an assumed
  48kHz item rate, since this device has no way to know a stream's
  DMAMUX-routed peripheral) plus real NVIC wiring for all 16 stream IRQs
  in `hw/arm/gnw_h7b0_soc.c`.
- Also fixed along the way: ADC `CR.ADCAL` never self-clearing (hung
  `patched-zelda-bank1.bin`'s ADC calibration); RTC `CR.ALRAE/ALRBE/WUTE`
  never setting their matching `SR` flag (hung the same image's next
  boot stage); TIM2-block `CR1.CEN` now drives a real per-instance
  counter instead of being inert (not the actual speed-bug root cause,
  but a real correctness gap in its own right).
- Added a real ticking RTC calendar (`TR`/`DR` now reflect actual
  elapsed time, seeded from host wall-clock at reset) — `gnw-chainloader`
  has a real clock display that only ever reads it, never sets it.
- Fixed battery always reading 0%: both firmwares read ADC1 channel 4
  (no I2C fuel gauge) with the same raw-value-to-percentage thresholds;
  `gnw_h7b0_adc.c` now returns a fixed value corresponding to 100% on
  `CR.ADSTART`, with a real (newly NVIC-wired) completion IRQ for
  retro-go-sd's interrupt-driven read path.
- Workflow: `sdcard.img` (repo root, full `dd` copy of the user's real
  SD card) is now the standard SD test fixture, via the same
  qcow2-overlay trick as the real-`/dev/mmcblk0` workflow. Always
  sample PC ~5x with a real `continue` (not just gdbstub re-attach,
  which re-halts on every attach and can look identical to a real hang)
  before concluding a run is stuck, and before screenshotting/asking the
  user to look at anything.

## 2026-07-10 (part 2 — gamepad, real-SD debugging, DMA2D)

Session picked up controller input, then chased real-hardware-SD testing
through five real hangs/crashes. See
`docs/session-2026-07-10-part3-gamepad-sd-debugging.md` for the full
narrative, the debugging technique used for each (mostly disassembly-only,
no debug symbols), and the "status flag never set → infinite poll" bug
pattern that accounted for most of them.

- Real xpad/gamepad support: `ui/sdl2-gamepad.c` polls SDL2's game
  controller API and synthesizes qcode key events via
  `qemu_input_event_send_key_qcode()`, reusing `gnw_h7b0_gpio.c`'s
  existing keyboard-based button dispatch untouched. D-pad + face buttons
  only (no analog stick support yet). Verified against a Hyperkin Xenon
  controller.
- Fixed `hw/misc/gnw_h7b0_spi.c`'s SD-card mode: `SR.RXP` was never set on
  a completed byte transfer, only `TXP`/`EOT`. Real firmware's
  `HAL_SPI_TransmitReceive()` polls `RXP` (not `EOT`) before reading each
  received byte, so every real SD response silently timed out and read
  back stale stack garbage instead of the card's actual byte.
- Fixed `hw/sd/ssi-sd.c` (upstream QEMU code): CMD58 (READ_OCR)'s R1
  response byte was hardcoded to `1` (idle) unconditionally. Real cards
  clear that bit once ACMD41 succeeds; hardcoding it stuck every SDv2
  card's voltage-negotiation check in "still idle" forever, so
  `sdcard_detect()` never found a card — independent of SPI1's RXP fix
  above, and independent of real vs. synthetic card images.
- Real DMA2D (Chrom-ART) device added: `hw/display/gnw_h7b0_dma2d.c` +
  header. R2M/M2M/M2M_PFC/M2M_BLEND modes; ARGB8888/RGB565/ARGB1555/
  ARGB4444 (+ L8-via-CLUT input); clean-room pixel math (per this repo's
  licensing note on `mk-snes/gnw-mk`, referencing only
  `../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/dma2d_emu.h`
  for register/mode semantics). Fixes a real BusFault in
  `HAL_DMA2D_Init()` — DMA2D was previously entirely unmapped.
- Fixed `hw/misc/gnw_h7b0_jpeg.c`: the real "start decode" trigger is
  `CONFR0.START`, not `CR` (CR is codec-enable/interrupt-enable, set once
  and left alone) — the initial fix hooked the wrong register. On a real
  `CONFR0.START` write, now fakes an instant "done" completion (`SR.EOCF`
  set, `OFNEF`/`OFTF`/`COF`/`IFTF`/`IFNFF` cleared) so
  `HAL_JPEG_Decode()`'s poll loop (and its input-refill sub-loop) actually
  terminates instead of walking a buffer off the end of AXISRAM. Decoded
  output is garbage (no real codec), but that's a correctness gap for
  later, not a crash.
- Fixed `hw/misc/gnw_h7b0_tim2.c` (covers the whole TIM2-TIM14 block):
  `EGR.UG` (force update event) now sets `SR.UIF`, applied generically at
  the standard 0x400-per-instance stride so it covers every timer in the
  block. Found via a silent black-screen hang in a patched, symbol-less
  firmware image — pure disassembly tracing (`r0` resolved to TIM5's base
  address) — a real hardware-timer busy-wait (`EGR.UG=1` then spin on
  `SR.UIF`) never terminated since nothing ever set `UIF`.
- Fixed `hw/misc/gnw_h7b0_rtc.c`'s `ICSR` write handler: it unconditionally
  rewrote the whole register to `(INIT)|INITF|RSF|INITS` on every write,
  silently zeroing `WUTWF`/`ALRBWF`/`ALRAWF` (bits 2/1/0, all set in
  `ICSR`'s real `0x7` reset value) after the very first write.
  `HAL_RTC_SetAlarm_IT()` polls `ALRAWF` before reprogramming the alarm
  register; once zeroed it never returned, hanging in an endless
  `WPR`/`CR`/`SCR` unlock-retry sequence before LTDC ever got configured.
  Same "instant ready" simplification as the register's other bits.
- Workflow: booting against a **real physical SD card** (`-drive
  if=sd,format=raw,file=/dev/mmcblk0`) hits two real constraints, not
  QEMU bugs: (1) QEMU's SD model requires an exact power-of-2 backing
  size, but real cards' raw byte length essentially never is one — wrap
  the real device in a qcow2 overlay with a full power-of-2 virtual size
  (`qemu-img create -f qcow2 -b /dev/mmcblk0 -F raw overlay.qcow2 8G`),
  which also means writes land in the overlay, not the physical card, no
  read-only mode needed to avoid touching real data. (2) the device node
  is normally `root:disk` and unwritable by a regular user — grant access
  via a one-off `chown root:plugdev` rather than a permanent group change
  if that's not wanted.

## 2026-07-10

- Added real button input (`hw/misc/gnw_h7b0_gpio.c`): a
  `QemuInputHandler` maps keyboard presses (and, best-effort, mouse-
  button events) onto the correct GPIO port's `IDR` bit per
  `gnw-chainloader`'s real button-pin table. Verified end-to-end by
  moving the chainloader menu cursor with a keyboard press. Real
  xpad-style gamepad support is blocked on a QEMU-version limitation,
  not a design gap: the pinned v9.2.4 base has no SDL2 joystick
  backend at all, so no real controller's buttons can reach any QEMU
  device yet — see STATUS.md.

- Fixed `hw/misc/gnw_h7b0_spi.c`'s `SR.TXP` being incorrectly gated on
  `CR1.SPE`: real hardware's TXP reflects TxFIFO occupancy and is set
  regardless of whether the peripheral is enabled. The old model only
  worked for `gnw-chainloader`'s SPI1 pattern (sets `SPE` immediately
  before every byte, only ever polls `EOT`); retro-go-sd's HAL-based
  `user_diskio_spi.c` polls `TXP` *before* `HAL_SPI_Transmit()` ever
  sets `SPE`, hanging forever. `gw_retro_go_sd_bank1.elf` now boots
  all the way to its own real UI ("No SD CARD found" screen).

- Added real LTDC vblank/line-interrupt modeling
  (`hw/display/gnw_h7b0_ltdc.c`, IRQ wired to NVIC's `LTDC_IRQn`=88 in
  `hw/arm/gnw_h7b0_soc.c`): a plain ~60Hz `QEMUTimer` now drives real
  `IER`/`ISR`/`ICR` registers instead of leaving them as inert shadow
  regs. Fixed both `gnw-chainloader`'s menu-flicker (its `gui_refresh()`
  was grabbing in-progress-redraw frames because its vblank wait never
  waited on anything real) and a hard, permanent hang in retro-go-sd's
  boot-logo animation (`lcd_wait_for_vblank()`).

- Fixed a false "button held" bug in `hw/misc/gnw_h7b0_gpio.c`: `IDR`
  was a zero-initialized plain-RAM placeholder, and since real G&W
  buttons are active-low, that read as *every* button permanently
  pressed — silently forcing `gnw-chainloader`'s boot-time "God Mode"
  button-override straight into an OFW-reflash screen on every launch.
  `IDR` now resets to `0xFFFF` per port (11 ports, A-K) instead of 0.

- Added a real hardware CRC-32 unit (`hw/misc/gnw_h7b0_crc.c`,
  `0x40023000`): a bit-serial engine matching the documented STM32
  algorithm exactly (configurable poly/init/reversal via
  `POL`/`INIT`/`CR`). `gnw-chainloader`'s `ofw_crc32()` uses this
  peripheral, not software, to verify OSPI flash content against
  baked-in checksums before trusting it — a plain-RAM stub always
  read `DR` back as 0 and made every verification fail silently.

- Added real OCTOSPI indirect-mode command decoding
  (`hw/misc/gnw_h7b0_ospi.c`): `RDID`/`RDSR`/`RDCR`/`SFDP`/`WREN`/
  `RSTEN`/`RST` now answer as a synthetic Macronix MX25U51245G
  (64MB, JEDEC `C2 25 3A`, matching `EXTFLASH_SIZE`'s existing 64M
  placeholder). Fixes retro-go's `OSPI_Init()` hard-`assert(!"Can't
  communicate with the external flash!")`.

- Added plain-RAM placeholders for TAMP (`0x58004400`), WWDG
  (`0x50003000`), and the TIM2/3/4/5/6/7/12/13/14 block
  (`0x40000000`-`0x400027FF`) — all found entirely unmapped (the
  "Lockup: can't escalate" abort pattern) while boot-testing a real
  OEM firmware image (`patched-zelda-bank1.bin`, real Zelda firmware
  with a Marian-Muller-style bank-swap patch) for the first time,
  combined with a real retro-go Bank2 image and the existing extflash
  backup. A scary-looking fault cascade this surfaced
  (`PC=0x0801ad68`, `LR=0xffffffe9`, a long tail of `Invalid read at
  0xFFFFFFEx`) turned out to be a downstream symptom of the WWDG gap,
  not a separate bug — gone entirely once WWDG was mapped.

- Added a real LTDC device (`hw/display/gnw_h7b0_ltdc.c`): Layer1/
  RGB565-only, reads the guest framebuffer each frame and blits it
  (2x nearest-neighbor upscaled) into a real QEMU display window.
  Fixed a row-pitch bug along the way (`LxCFBLR`'s `CFBLL` field isn't
  the real stride, `CFBP` is). Also upgraded SPI2 from a plain-RAM stub
  to a real device (`hw/misc/gnw_h7b0_spi.c`) after the LCD-panel
  init-command path hung the same way OSPI/ADC previously did.
  `gnw-chainloader` now boots and runs continuously with a real window
  showing the LCD console output.

- Booted a real `gnw-chainloader` firmware image end-to-end for the
  first time and fixed the resulting chain of boot-path gaps: RCC's
  `CSR`/`BDCR` LSI/LSE ready-bit mirroring; new real devices for PWR
  (`hw/misc/gnw_h7b0_pwr.c`), OCTOSPI1/2 (`gnw_h7b0_ospi.c`), and
  ADC1/2 (`gnw_h7b0_adc.c`), each mirroring a hardware-set status bit
  real firmware polls after enabling the peripheral; plain-RAM
  placeholders for DBGMCU, the flash controller register block, FMC,
  GPIOA-K, CRS, the OCTOSPI IO manager, and SPI2. Boot now reaches real
  LTDC init and stops cleanly there (BusFault caught by the firmware's
  own crash handler) — see STATUS.md for the full gap-by-gap
  breakdown and what's next (LTDC device model).

- Phase 1 boot path resolved: the kernel now loads at flash bank 1
  (`0x08000000`) instead of ITCM, with the ARMv7M CPU's `init-nsvtor`
  property pointed there so reset reads the initial SP/PC from flash's
  vector table — modeling real hardware's BOOT_ADD address-0 remap (or
  the gnwmanager debug-probe dev-flow path) without a fake alias memory
  region. Note: it's `init-nsvtor`, not `init-svtor` — Cortex-M7 has no
  TrustZone-M, so the `-s-` (secure) variant is a silent no-op on this
  core. Verified via a hand-built flash-linked test kernel under gdb
  (`-s -S`): SP/PC load as `0x20020000`/`0x0800000c` from the flash
  vector table, and a store instruction a few steps in lands correctly
  in AXI SRAM.

- Repo initialized as a fork of upstream QEMU. Remotes set: `origin` =
  `slash-proc/gwemu`, `upstream` = `qemu/qemu`. Fetched full upstream
  history/tags locally; pushed only the `v9.2.4` stable tag to `origin` as
  the pinned base (deliberately not syncing all of upstream master/tags to
  the fork).
- Initial documentation established: `CLAUDE.md`, `STATUS.md`,
  `CHANGELOG.md`, `docs/roadmap.md`.
- Phase 0 complete: `gnw-h7b0` machine added (`hw/arm/gnw_h7b0.c`,
  `hw/arm/gnw_h7b0_soc.c`) — bare Cortex-M7 + DTCM + AXI SRAM, no
  peripherals. Verified booting a hand-built spin-loop ELF via gdb
  (`-s -S`): reset SP/PC resolve correctly and PC advances on single-step.
  Branch `gnw-h7b0` (based on tag `v9.2.4`) pushed to `origin`.
- Added `rm0455.pdf` (STM32H7B0 reference manual) to the repo; used it to
  build out the real SRAM/flash memory map (ITCM, DTCM, AXI SRAM1/2/3,
  AHB SRAM1/2, SRD SRAM, backup SRAM, internal flash banks, external OSPI
  flash placeholder) in `gnw_h7b0_soc.{c,h}`, replacing Phase 0's single
  DTCM/AXISRAM blob. Discovered and documented (`docs/h7b0-flash-
  discrepancy.md`) that RM0455 is wrong about internal flash size/layout
  on real H7B0 hardware (says 128K single-bank; real silicon is 2x256K
  dual-bank, per community/project-owner knowledge) — deliberately
  overrode the reference manual here.
- Replaced the Phase-0 "alias AXI SRAM at address 0" boot hack with
  loading the test kernel directly into ITCM (genuinely mapped at `0x0`
  on real hardware) — architecturally correct rather than a stand-in.
  Re-verified boot via gdb. Flagged an open question: real firmware >64K
  needs flash-backed boot via BOOT_ADD option-byte selection, not yet
  modeled.
- Added `scripts/fetch-sdk.sh`, pulling the real STM32CubeH7 HAL driver +
  device CMSIS source into gitignored `sdk/`, pinned to the same versions
  `game-and-watch-retro-go-sd` builds real firmware against.
- Implemented and verified a minimal RCC device stub
  (`hw/misc/gnw_h7b0_rcc.{c,h}`) mirroring RCC_CR ON->RDY bits and
  RCC_CFGR SW->SWS so real firmware's clock-init polling doesn't hang.
  Verified via a real CPU-executed test program. **NOT YET COMMITTED**
  as of this entry — see STATUS.md "Uncommitted work" section; a Claude
  Code session restart interrupted the commit step (unrelated
  auto-mode-classifier false positives blocking git commands, not a
  problem with the code itself).
- Added Xbox/xpad-style gamepad support (`ui/sdl2-gamepad.{c,h}`): polls
  SDL2's `SDL_GameController` API and synthesizes keyboard `InputEvent`s
  via `qemu_input_event_send_key_qcode()`, reusing
  `gnw_h7b0_gpio.c`'s existing keyboard qcode mapping unchanged (same
  shortcut xemu takes against its own SDL/ImGui UI, adapted to stock
  QEMU's `ui/sdl2*.c`). First connected controller only, fixed mapping
  (d-pad -> arrows, A/B -> Z/X, Start -> Return, Back -> Right-Shift,
  Guide/right-shoulder -> Escape/Pause); no qapi/ui.json changes. Builds
  cleanly under the existing `sdl.found()` gate; not yet tested against
  physical hardware.
