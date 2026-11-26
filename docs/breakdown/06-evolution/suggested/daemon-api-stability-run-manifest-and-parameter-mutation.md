---
id: IMP-054
type: imp
title: Daemon API stability, run manifest, and parameter mutation
summary: Stabilize the daemon contract, complete run discovery beyond the active run identity, and add parameter mutation exposed by the GUI's limits
date: 2026-11-26
status: suggested
---

# Daemon API stability, run manifest, and parameter mutation

## Purpose

The engineer planning the next engine increment uses this node to see the daemon and API work the GUI exposed as missing; without it, frontend limits are mistaken for frontend bugs.

---

**Question:** What engine-side work closes the GUI's control, discovery, and parameter limits at the daemon boundary rather than in the frontend?

**Evidence:** The frontend (`INFO-044`) consumes `ssp4simd` and result artifacts. The daemon now exposes the active run's `session_id` and absolute `artifacts` in `state`/`done`, so a client can attach mid-run without scanning; the API still has no stability contract, the artifact snapshot is not a complete run manifest for historical runs, and discovery of runs not currently loaded still relies on recency. Parameters are startup-only, with no set-parameter command. `IMP-037` covers public-API lifecycle controls; this is its daemon-side counterpart for protocol stability, discovery, and parameter mutation.

**Change:** Publish a versioned daemon API contract for commands, events, and compatibility; extend the delivered run identity/artifact snapshot into a full run manifest if historical discovery still needs one; add a set-parameter command with defined live-versus-restart semantics.

**Risk:** Medium — the work touches the daemon protocol and the engine mutation path; it must not add a network listener to the engine or carry result values over the control channel.

**Acceptance:** The active run and its artifacts are discoverable without recency inference (delivered); historical runs are discoverable without recency inference; the protocol has a compatibility contract; parameter mutation has defined command and state semantics; the engine opens no network listener.
