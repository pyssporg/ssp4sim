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

**Evidence:** The frontend (`INFO-044`) consumes `ssp4simd` and result artifacts. HEAD delivers the active run's `session_id` and absolute `artifacts` in `state`/`done` (`daemon.cpp:42-43,62-63,404-405`), so a client can attach mid-run without scanning. It also delivers affine `set_override`/`clear_override`/`list_overrides` for input/output variables (`simulator_core.hpp:111-117`); these are not parameter mutation, and parameters remain startup-only. The protocol enforces `v:1` (`protocol.cpp:40-47`) but has no compatibility policy, and historical discovery still sorts result roots by mtime (`store.py:138-171`). `IMP-037` covers public-API lifecycle controls; this is its daemon-side counterpart for protocol stability, discovery, and parameter mutation.

**Change:** Publish a compatibility policy for the `v:1` daemon protocol (additive fields, event evolution, deprecation, version bumps); define a persisted run manifest if historical discovery still cannot avoid recency inference; add a set-parameter command with defined live-versus-restart semantics.

**Risk:** Medium — the work touches the daemon protocol and the engine mutation path; it must not add a network listener to the engine or carry result values over the control channel.

**Acceptance:** The active run and its artifacts are discoverable without recency inference (delivered); historical runs are discoverable without recency inference; the protocol has a published compatibility policy; parameter mutation has defined command and state semantics; the engine opens no network listener (delivered).
