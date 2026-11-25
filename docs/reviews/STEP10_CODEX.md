# Step 10 joint review: raw Codex output

Review of `c15f6b2` (integ/bt-all-local). Final answer only (the exploration transcript is omitted); local paths
stripped. Triage: [STEP10_JOINT_REVIEW.md](STEP10_JOINT_REVIEW.md).

```json
[
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_xrec_fw.c:361",
    "issue": "Temporary-file cleanup can free clusters still owned by the published bond file, XREC key, or headphone settings file.",
    "failure_scenario": "exfat_replace_file publishes the destination's new cluster pointer at open_fw/src/exfat.c:2188 or :2219, then unlinks the source name at :2258. Power loss between those operations leaves both names pointing to the same clusters. The next save deletes the temporary name, and exfat_delete_file frees those shared clusters at open_fw/src/exfat.c:1918. A later allocation can overwrite the published file. An I/O error after publication can reach the same destructive cleanup immediately at open_fw/bt_port/bt_xrec_fw.c:369.",
    "fix": "Make replacement recovery distinguish an unpublished temporary file from a second directory entry for the published data. Unlink an alias without freeing its clusters. Two independently allocated, generation-numbered slots would also avoid this shared-cluster window."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1473",
    "issue": "Headphones mode advertises DisplayYesNo but confirms SSP Numeric Comparison without showing the number or receiving a user answer.",
    "failure_scenario": "The user selects a display-capable audio device. Bluedroid delivers a comparison value, and the recorder replies yes as soon as the address matches the selection and the cached gate is open. Even if the two devices display different values, the recorder's side approves. The resulting authenticated key classification overstates what the user verified.",
    "fix": "Use a pending prompt bound to the peer, comparison value, and deadline, as phone mode does. Confirm only after explicit approval. Reject Numeric Comparison if the headphone UI cannot support it."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:523",
    "issue": "The headphone pairing decision uses a cached entropy verdict, allowing pairing after the ROM has fallen back to its legacy RNG.",
    "failure_scenario": "A pairing window opens while the RNG pool is healthy. Before the next app poll, a security-sensitive ROM draw exhausts the pool and returns legacy randomness at open_fw/bt_port/bt_rom_rng.c:147. The higher-priority BTC task can still approve Just Works using the old gate_ok value. The success handler at open_fw/bt_port/bt_hp_bluedroid.c:1316 retains the selected peer's bond without rechecking starvation.",
    "fix": "Latch starvation at the draw site and consult that latch directly at confirmation. Check it again before retaining a newly generated key. Keep the latch readable without taking the entropy mutex inside a critical section."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:518",
    "issue": "The Just Works hook defaults to approval in phone mode and during headphone teardown.",
    "failure_scenario": "Phone mode has no headphone session object, so the hook returns true even with the pairing window closed. The controller can complete Just Works and Bluedroid can store a key before the phone app rejects the result at open_fw/bt_port/bt_xrec_bluedroid.c:1000. Closing headphone callbacks at open_fw/bt_port/bt_hp_bluedroid.c:1932 creates the same default-allow interval during disable. The phone SPP MITM check still independently protects XREC access.",
    "fix": "Default the hook to rejection. Allow Just Works only for a live headphone session with an explicitly selected peer, an unexpired window, and a current healthy pairing gate."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_ctrl_rtos.c:106",
    "issue": "A succession of short controller runs can starve the recorder and Bluetooth host indefinitely while passing the production hang check.",
    "failure_scenario": "If the ROM continues reporting work through every pass budget, bt_ctrl_run returns more at open_fw/bt_port/bt_ctrl.c:1858. The priority-8 controller immediately runs again with a zero wait. Each invocation can finish below the single-run limit at open_fw/bt_port/bt_ctrl_rtos.c:192 while the priority-1 recorder and lower-priority host tasks receive no CPU. The cumulative CPU guard exists, but only the probe arms it. Recording DMA can continue while the app cannot drain it.",
    "fix": "Enforce a cumulative controller CPU budget in normal sessions. After sustained budget exhaustion, provide a bounded scheduling opportunity for lower-priority tasks or have the timer daemon invoke the existing demotion and recovery path."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_xrec_fw.c:403",
    "issue": "The removable card contains both the reusable XREC authentication key and the Bluetooth bond material in readable form.",
    "failure_scenario": "Someone who can read the card obtains the complete 32-byte application key from cassini.key. The bond serializer copies its values verbatim at open_fw/bt_port/bt_store.c:346 and appends an unkeyed digest at :349. Copying the files therefore discloses credentials that remain valid after the card is returned. The digest also cannot authenticate a replacement bond image to this recorder.",
    "fix": "Keep credentials in device-private storage, or protect card-backed records with authenticated encryption under a device-private key. Bind the protected record to its purpose and mode so a headphone record cannot be substituted for a phone record."
  },
  {
    "severity": "medium",
    "file": "open_fw/third_party/bluedroid/host/bluedroid/btc/profile/std/spp/btc_spp.c:637",
    "issue": "A single allocation failure while handing a completed RFCOMM write to BTC can permanently stall an otherwise connected phone transfer. Patch 0024 corrects the copy size but leaves this failure path.",
    "failure_scenario": "SPP accepts a write and sets slot->is_writing at :405. Its completion reaches the BTA callback, but btc_transfer_context fails under heap pressure and :641 only logs the failure. The BTC handler at :1314 never clears is_writing, and subsequent drains return immediately at :385. XREC also retains the outstanding bytes and eventually stops submitting at open_fw/bt_port/bt_xrec_bluedroid.c:570. Heap recovery alone cannot restart the transfer.",
    "fix": "Guarantee delivery of write completions using reserved per-slot state or a by-value work item. If completion accounting cannot be recovered, close the affected connection and reset its transfer state."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2419",
    "issue": "Forget can report success and erase the only UI reference to a headset while retaining its link key.",
    "failure_scenario": "The screen offers Forget whenever has_last is set, including with Bluetooth off, at open_fw/src/main.c:1813. Bond removal runs only in READY. Even in READY, an immediate removal error is merely logged. In both cases :2429 clears the remembered address, saves the settings, and returns success, so the user loses the menu entry needed to retry.",
    "fix": "Retain the address until removal is confirmed and persisted. With Bluetooth off, either safely update the headphone bond store or persist a pending removal that runs before reconnection."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_fw.c:404",
    "issue": "XREC acknowledges a new binding even when its key could not be saved, allowing an old binding to return after restart.",
    "failure_scenario": "A user rebinds to a new phone while the card is full or a write fails before publication. The save callback returns void and only logs the error. open_fw/src/xrec_proto.c:342 installs the new key in RAM and :350 sends OK,PAIR. On the next phone-mode entry, open_fw/bt_port/bt_xrec_fw.c:389 reloads the previous key. The new phone loses access, and the old application credential is valid again for a peer that passes the Bluetooth link checks.",
    "fix": "Return persistence status to the pairing handler. For durable rebinding, commit the new key before switching the active key and acknowledging success. Make a deliberately session-only binding explicit."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_fw.c:914",
    "issue": "Starting a session can overwrite newer, dirty RAM bonds with an older SD image after a failed final flush.",
    "failure_scenario": "Pair or remove a peer, then encounter an SD write failure through shutdown. The failure leaves the store dirty, but session_end resets the retry count at :1019. Re-entering the same mode calls bonds_load unconditionally. A readable older image reaches bt_store_load, which clears the RAM entries at open_fw/bt_port/bt_store.c:372. This can discard a new bond or restore one the user removed without any reboot.",
    "fix": "Track which mode and volume own an unsaved generation. Retry or explicitly resolve its failed commit before loading an older image or resetting the store on a mode change."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_bluedroid.c:517",
    "issue": "A failed SPP disconnect request leaves the sole phone session permanently marked as closing.",
    "failure_scenario": "An RX overflow or write error calls conn_close_request. It marks both app and callback state as closing before asking Bluedroid to disconnect. If the request cannot be queued, :519 only logs it. Subsequent close requests return at :502, input is dropped, and new sessions are refused at :1130. Recovery depends on the peer disconnecting independently or the user cycling Bluetooth.",
    "fix": "Track close-request submission separately from closing policy. Retry submission with a deadline while continuing to reject application traffic, then escalate to ACL close or session recovery if no close event arrives."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2098",
    "issue": "A reconnect timeout can cancel using the previous connection's A2DP handle.",
    "failure_scenario": "Connect headset A and disconnect it. CONNECTED stores its handle at :1128, but on_link_down at :1073 does not invalidate it. Start a connection to B and let paging time out before B has a handle. The timeout selects disconnect-by-handle using A's stale value, then records B as cancelled. The manual cancellation path at :2396 makes the same choice, so B's attempt can remain active.",
    "fix": "Invalidate conn_hdl on link loss and before every new attempt. Cancel a CONNECTING attempt by its target address until that specific attempt receives CONNECTED."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:586",
    "issue": "A stale A2DP disconnect stops the current audio transmitter before the app checks which peer disconnected.",
    "failure_scenario": "A timed-out connection to A reports late while B is the current stream. A's DISCONNECTED callback stops the shared tick, clears its ACL filter, and clears cached codec state. The app later recognizes A as stale and returns at :1175. Its state can still say B is STARTED although B's transmitter has stopped. The CONNECTED callback likewise installs a handle at :568 before the app's late-connection check.",
    "fix": "Bind transmitter mutations to the active peer, handle, and connection generation in the callback. A stale connection should clean up only its own state."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1943",
    "issue": "Headphone shutdown treats profile teardown failure as a successful, restartable Bluetooth-off state.",
    "failure_scenario": "If A2DP deinitialization never confirms, hp_profile_down returns an error at :1882 and the worker maps it to 5 at open_fw/bt_port/bt_xrec_fw.c:774. Shutdown specifically exempts 5, logs that a recorder restart is required, then sets OFF and returns success at :1975. The UI permits another start even though the previous profile lifecycle did not complete.",
    "fix": "Propagate profile teardown failure. Restore and verify the profile's initialization state before allowing another start, or latch a restart-required state and expose it to the UI."
  },
  {
    "severity": "medium",
    "file": "open_fw/src/player.c:996",
    "issue": "After a headphone-session fault detaches the output, Play can resume a file into a route that accepts zero frames.",
    "failure_scenario": "During headphone-only playback, session teardown changes the route to NONE at open_fw/src/audio_out.c:77. The player pauses at open_fw/src/player.c:1003. On the next Play press, the disconnected-headphone guard is skipped because aout_is_bt() is now false. The player unpauses, but aout_write returns zero for NONE at open_fw/src/audio_out.c:207. The current file makes no progress even if the session later reattaches, because attachment alone does not select a route.",
    "fix": "Keep the file's intended route independently of the currently attached output. On resume, reopen a usable route or keep the player paused with a disconnected status."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2079",
    "issue": "The discovery timeout can leave the headphone screen searching indefinitely.",
    "failure_scenario": "Start discovery and select a headset, then lose the discovery-stopped notification or encounter an error posting cancellation. The code sets scan_cancel before cancellation and leaves scanning set. The timeout at :2078 only runs while scan_cancel is clear, and a queued pick at :2083 waits for scanning to clear. A new search is refused at :2281.",
    "fix": "Give cancellation its own completion deadline. Retry failed submissions, then reconcile discovery state with the controller or recover the session before processing the pending pick."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1483",
    "issue": "On bench images, enabling legacy PIN 0000 for one unattended run also enables it for later manual headphone sessions in the same boot.",
    "failure_scenario": "bt_bench_hp_prepare copies hp_pin into static tuning at open_fw/bt_port/bt_bench_fw.c:271. A run ends without rebooting. The headphone finish path at :575 and bench cleanup at :685 leave that tuning in place. A later manual pairing with a selected legacy headset therefore answers 0000. Even passing NULL to the tuning setter would leave the stored pin0000 field intact.",
    "fix": "Clear the entire headphone tuning structure when the bench run ends, and require an active bench run at the point where a legacy PIN is approved."
  },
  {
    "severity": "low",
    "file": "open_fw/bt_port/bt_xrec_fw.c:224",
    "issue": "All recorders without a valid eFuse identity use the same fallback Bluetooth address.",
    "failure_scenario": "Pair two such recorders with the same phone. Both present the same controller address but establish different link keys, so the phone cannot maintain independent bond identities for them. Reconnecting to the first device can require pairing again.",
    "fix": "Derive a stable locally administered address from a device-unique identifier, or generate and persist a unique fallback address in device-local storage."
  }
]
```

**Summary.** The highest-impact findings are the interrupted exFAT replacement that can free live clusters, pairing decisions that approve without current authorization or entropy checks, and controller scheduling that can starve recording. The XREC link and application authentication checks are separate: the phone-mode Just Works finding establishes unintended pairing, not unauthenticated recording access. Headphone connection and streaming findings apply to builds with the inquiry and paging gates enabled. This was a read-only static review; the failure scenarios were traced in source and were not reproduced on silicon.
tokens used
181,111
```json
[
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_xrec_fw.c:361",
    "issue": "Temporary-file cleanup can free clusters still owned by the published bond file, XREC key, or headphone settings file.",
    "failure_scenario": "exfat_replace_file publishes the destination's new cluster pointer at open_fw/src/exfat.c:2188 or :2219, then unlinks the source name at :2258. Power loss between those operations leaves both names pointing to the same clusters. The next save deletes the temporary name, and exfat_delete_file frees those shared clusters at open_fw/src/exfat.c:1918. A later allocation can overwrite the published file. An I/O error after publication can reach the same destructive cleanup immediately at open_fw/bt_port/bt_xrec_fw.c:369.",
    "fix": "Make replacement recovery distinguish an unpublished temporary file from a second directory entry for the published data. Unlink an alias without freeing its clusters. Two independently allocated, generation-numbered slots would also avoid this shared-cluster window."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1473",
    "issue": "Headphones mode advertises DisplayYesNo but confirms SSP Numeric Comparison without showing the number or receiving a user answer.",
    "failure_scenario": "The user selects a display-capable audio device. Bluedroid delivers a comparison value, and the recorder replies yes as soon as the address matches the selection and the cached gate is open. Even if the two devices display different values, the recorder's side approves. The resulting authenticated key classification overstates what the user verified.",
    "fix": "Use a pending prompt bound to the peer, comparison value, and deadline, as phone mode does. Confirm only after explicit approval. Reject Numeric Comparison if the headphone UI cannot support it."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:523",
    "issue": "The headphone pairing decision uses a cached entropy verdict, allowing pairing after the ROM has fallen back to its legacy RNG.",
    "failure_scenario": "A pairing window opens while the RNG pool is healthy. Before the next app poll, a security-sensitive ROM draw exhausts the pool and returns legacy randomness at open_fw/bt_port/bt_rom_rng.c:147. The higher-priority BTC task can still approve Just Works using the old gate_ok value. The success handler at open_fw/bt_port/bt_hp_bluedroid.c:1316 retains the selected peer's bond without rechecking starvation.",
    "fix": "Latch starvation at the draw site and consult that latch directly at confirmation. Check it again before retaining a newly generated key. Keep the latch readable without taking the entropy mutex inside a critical section."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:518",
    "issue": "The Just Works hook defaults to approval in phone mode and during headphone teardown.",
    "failure_scenario": "Phone mode has no headphone session object, so the hook returns true even with the pairing window closed. The controller can complete Just Works and Bluedroid can store a key before the phone app rejects the result at open_fw/bt_port/bt_xrec_bluedroid.c:1000. Closing headphone callbacks at open_fw/bt_port/bt_hp_bluedroid.c:1932 creates the same default-allow interval during disable. The phone SPP MITM check still independently protects XREC access.",
    "fix": "Default the hook to rejection. Allow Just Works only for a live headphone session with an explicitly selected peer, an unexpired window, and a current healthy pairing gate."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_ctrl_rtos.c:106",
    "issue": "A succession of short controller runs can starve the recorder and Bluetooth host indefinitely while passing the production hang check.",
    "failure_scenario": "If the ROM continues reporting work through every pass budget, bt_ctrl_run returns more at open_fw/bt_port/bt_ctrl.c:1858. The priority-8 controller immediately runs again with a zero wait. Each invocation can finish below the single-run limit at open_fw/bt_port/bt_ctrl_rtos.c:192 while the priority-1 recorder and lower-priority host tasks receive no CPU. The cumulative CPU guard exists, but only the probe arms it. Recording DMA can continue while the app cannot drain it.",
    "fix": "Enforce a cumulative controller CPU budget in normal sessions. After sustained budget exhaustion, provide a bounded scheduling opportunity for lower-priority tasks or have the timer daemon invoke the existing demotion and recovery path."
  },
  {
    "severity": "high",
    "file": "open_fw/bt_port/bt_xrec_fw.c:403",
    "issue": "The removable card contains both the reusable XREC authentication key and the Bluetooth bond material in readable form.",
    "failure_scenario": "Someone who can read the card obtains the complete 32-byte application key from cassini.key. The bond serializer copies its values verbatim at open_fw/bt_port/bt_store.c:346 and appends an unkeyed digest at :349. Copying the files therefore discloses credentials that remain valid after the card is returned. The digest also cannot authenticate a replacement bond image to this recorder.",
    "fix": "Keep credentials in device-private storage, or protect card-backed records with authenticated encryption under a device-private key. Bind the protected record to its purpose and mode so a headphone record cannot be substituted for a phone record."
  },
  {
    "severity": "medium",
    "file": "open_fw/third_party/bluedroid/host/bluedroid/btc/profile/std/spp/btc_spp.c:637",
    "issue": "A single allocation failure while handing a completed RFCOMM write to BTC can permanently stall an otherwise connected phone transfer. Patch 0024 corrects the copy size but leaves this failure path.",
    "failure_scenario": "SPP accepts a write and sets slot->is_writing at :405. Its completion reaches the BTA callback, but btc_transfer_context fails under heap pressure and :641 only logs the failure. The BTC handler at :1314 never clears is_writing, and subsequent drains return immediately at :385. XREC also retains the outstanding bytes and eventually stops submitting at open_fw/bt_port/bt_xrec_bluedroid.c:570. Heap recovery alone cannot restart the transfer.",
    "fix": "Guarantee delivery of write completions using reserved per-slot state or a by-value work item. If completion accounting cannot be recovered, close the affected connection and reset its transfer state."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2419",
    "issue": "Forget can report success and erase the only UI reference to a headset while retaining its link key.",
    "failure_scenario": "The screen offers Forget whenever has_last is set, including with Bluetooth off, at open_fw/src/main.c:1813. Bond removal runs only in READY. Even in READY, an immediate removal error is merely logged. In both cases :2429 clears the remembered address, saves the settings, and returns success, so the user loses the menu entry needed to retry.",
    "fix": "Retain the address until removal is confirmed and persisted. With Bluetooth off, either safely update the headphone bond store or persist a pending removal that runs before reconnection."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_fw.c:404",
    "issue": "XREC acknowledges a new binding even when its key could not be saved, allowing an old binding to return after restart.",
    "failure_scenario": "A user rebinds to a new phone while the card is full or a write fails before publication. The save callback returns void and only logs the error. open_fw/src/xrec_proto.c:342 installs the new key in RAM and :350 sends OK,PAIR. On the next phone-mode entry, open_fw/bt_port/bt_xrec_fw.c:389 reloads the previous key. The new phone loses access, and the old application credential is valid again for a peer that passes the Bluetooth link checks.",
    "fix": "Return persistence status to the pairing handler. For durable rebinding, commit the new key before switching the active key and acknowledging success. Make a deliberately session-only binding explicit."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_fw.c:914",
    "issue": "Starting a session can overwrite newer, dirty RAM bonds with an older SD image after a failed final flush.",
    "failure_scenario": "Pair or remove a peer, then encounter an SD write failure through shutdown. The failure leaves the store dirty, but session_end resets the retry count at :1019. Re-entering the same mode calls bonds_load unconditionally. A readable older image reaches bt_store_load, which clears the RAM entries at open_fw/bt_port/bt_store.c:372. This can discard a new bond or restore one the user removed without any reboot.",
    "fix": "Track which mode and volume own an unsaved generation. Retry or explicitly resolve its failed commit before loading an older image or resetting the store on a mode change."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_xrec_bluedroid.c:517",
    "issue": "A failed SPP disconnect request leaves the sole phone session permanently marked as closing.",
    "failure_scenario": "An RX overflow or write error calls conn_close_request. It marks both app and callback state as closing before asking Bluedroid to disconnect. If the request cannot be queued, :519 only logs it. Subsequent close requests return at :502, input is dropped, and new sessions are refused at :1130. Recovery depends on the peer disconnecting independently or the user cycling Bluetooth.",
    "fix": "Track close-request submission separately from closing policy. Retry submission with a deadline while continuing to reject application traffic, then escalate to ACL close or session recovery if no close event arrives."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2098",
    "issue": "A reconnect timeout can cancel using the previous connection's A2DP handle.",
    "failure_scenario": "Connect headset A and disconnect it. CONNECTED stores its handle at :1128, but on_link_down at :1073 does not invalidate it. Start a connection to B and let paging time out before B has a handle. The timeout selects disconnect-by-handle using A's stale value, then records B as cancelled. The manual cancellation path at :2396 makes the same choice, so B's attempt can remain active.",
    "fix": "Invalidate conn_hdl on link loss and before every new attempt. Cancel a CONNECTING attempt by its target address until that specific attempt receives CONNECTED."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:586",
    "issue": "A stale A2DP disconnect stops the current audio transmitter before the app checks which peer disconnected.",
    "failure_scenario": "A timed-out connection to A reports late while B is the current stream. A's DISCONNECTED callback stops the shared tick, clears its ACL filter, and clears cached codec state. The app later recognizes A as stale and returns at :1175. Its state can still say B is STARTED although B's transmitter has stopped. The CONNECTED callback likewise installs a handle at :568 before the app's late-connection check.",
    "fix": "Bind transmitter mutations to the active peer, handle, and connection generation in the callback. A stale connection should clean up only its own state."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1943",
    "issue": "Headphone shutdown treats profile teardown failure as a successful, restartable Bluetooth-off state.",
    "failure_scenario": "If A2DP deinitialization never confirms, hp_profile_down returns an error at :1882 and the worker maps it to 5 at open_fw/bt_port/bt_xrec_fw.c:774. Shutdown specifically exempts 5, logs that a recorder restart is required, then sets OFF and returns success at :1975. The UI permits another start even though the previous profile lifecycle did not complete.",
    "fix": "Propagate profile teardown failure. Restore and verify the profile's initialization state before allowing another start, or latch a restart-required state and expose it to the UI."
  },
  {
    "severity": "medium",
    "file": "open_fw/src/player.c:996",
    "issue": "After a headphone-session fault detaches the output, Play can resume a file into a route that accepts zero frames.",
    "failure_scenario": "During headphone-only playback, session teardown changes the route to NONE at open_fw/src/audio_out.c:77. The player pauses at open_fw/src/player.c:1003. On the next Play press, the disconnected-headphone guard is skipped because aout_is_bt() is now false. The player unpauses, but aout_write returns zero for NONE at open_fw/src/audio_out.c:207. The current file makes no progress even if the session later reattaches, because attachment alone does not select a route.",
    "fix": "Keep the file's intended route independently of the currently attached output. On resume, reopen a usable route or keep the player paused with a disconnected status."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:2079",
    "issue": "The discovery timeout can leave the headphone screen searching indefinitely.",
    "failure_scenario": "Start discovery and select a headset, then lose the discovery-stopped notification or encounter an error posting cancellation. The code sets scan_cancel before cancellation and leaves scanning set. The timeout at :2078 only runs while scan_cancel is clear, and a queued pick at :2083 waits for scanning to clear. A new search is refused at :2281.",
    "fix": "Give cancellation its own completion deadline. Retry failed submissions, then reconcile discovery state with the controller or recover the session before processing the pending pick."
  },
  {
    "severity": "medium",
    "file": "open_fw/bt_port/bt_hp_bluedroid.c:1483",
    "issue": "On bench images, enabling legacy PIN 0000 for one unattended run also enables it for later manual headphone sessions in the same boot.",
    "failure_scenario": "bt_bench_hp_prepare copies hp_pin into static tuning at open_fw/bt_port/bt_bench_fw.c:271. A run ends without rebooting. The headphone finish path at :575 and bench cleanup at :685 leave that tuning in place. A later manual pairing with a selected legacy headset therefore answers 0000. Even passing NULL to the tuning setter would leave the stored pin0000 field intact.",
    "fix": "Clear the entire headphone tuning structure when the bench run ends, and require an active bench run at the point where a legacy PIN is approved."
  },
  {
    "severity": "low",
    "file": "open_fw/bt_port/bt_xrec_fw.c:224",
    "issue": "All recorders without a valid eFuse identity use the same fallback Bluetooth address.",
    "failure_scenario": "Pair two such recorders with the same phone. Both present the same controller address but establish different link keys, so the phone cannot maintain independent bond identities for them. Reconnecting to the first device can require pairing again.",
    "fix": "Derive a stable locally administered address from a device-unique identifier, or generate and persist a unique fallback address in device-local storage."
  }
]
```

**Summary.** The highest-impact findings are the interrupted exFAT replacement that can free live clusters, pairing decisions that approve without current authorization or entropy checks, and controller scheduling that can starve recording. The XREC link and application authentication checks are separate: the phone-mode Just Works finding establishes unintended pairing, not unauthenticated recording access. Headphone connection and streaming findings apply to builds with the inquiry and paging gates enabled. This was a read-only static review; the failure scenarios were traced in source and were not reproduced on silicon.
