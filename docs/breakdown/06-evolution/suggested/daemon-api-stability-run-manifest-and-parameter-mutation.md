---
id: IMP-054
type: imp
title: Daemon API stability, run manifest, and parameter mutation
summary: Stabilize the daemon contract, add run manifest/discovery, and add parameter mutation exposed by the GUI's limits
date: 2026-11-26
status: suggested
---

# Daemon API stability, run manifest, and parameter mutation

## Purpose

The engineer planning the next engine increment uses this node to see the daemon and API work the GUI exposed as missing; without it, frontend limits are mistaken for frontend bugs.

---

**Question:** What engine-side work closes the GUI's control, discovery, and parameter limits at the daemon boundary rather than in the frontend?

**Evidence:** The frontend (`INFO-044`) consumes `ssp4simd` and result artifacts. The daemon exists only on `origin/external_control`; its API has no stability contract, results carry no run manifest, and discovery relies on recency. Parameters are startup-only, with no set-parameter command. `IMP-037` covers public-API lifecycle controls; this is its daemon-side counterpart for protocol stability, discovery, and parameter mutation.

**Change:** Publish a versioned daemon API contract for commands, events, and compatibility; add a run manifest so runs and artifacts are discoverable without recency heuristics; add a set-parameter command with defined live-versus-restart semantics.

**Risk:** Medium — the work touches the daemon protocol and the engine mutation path; it must not add a network listener to the engine or carry result values over the control channel.

**Acceptance:** Runs are discovered without recency inference; the protocol has a compatibility contract; parameter mutation has defined command and state semantics; the engine opens no network listener.
