classDiagram

class Simulator {
    Caller-facing facade and sole action producer: retains the creation config, posts Start/Pause/Resume/Stop/SetSpeed/Snapshot through SimulationRuntime's ControlChannel, and reads status through it. snapshot() returns bool; restore and reset rebuild through the private factories via the internal detail::RebuildAccess helper.
}

class SimulatorCore {
    Synchronous core: owns SSP import, graph, executors, recorder, simulation time, snapshot save/load, and one macro step; no thread, pacing, RunState, or command queue.
}

class SimulationRuntime {
    Owns the simulation side: the synchronous SimulatorCore, the run thread and future with a separate lifecycle mutex, the outer macro-step loop, and the single ControlChannel queue/status object; it consumes and applies actions and publishes state, and never posts.
}

class ControlChannel {
    Single shared object between caller and run thread: one mutex and one condition variable guard RunState, simulation time, speed, the applied sequence, the last error, and one time-ordered multimap of Start/Pause/Resume/Stop/SetSpeed/Snapshot Actions, plus a closed flag. Only the runtime writes RunState; the Simulator is the only producer. post() is fire-and-forget and post_and_wait() blocks for the applied result and is the only path that keeps a completion record.
}

class PacingClock {
    Stores speed and wall/simulation anchors and computes pure pacing deadlines; it owns no lock and no simulation time.
}

class FmuHandler {
    Loads FMU archives and prepares instances for execution.
}

class FmuInfo {
    Stores metadata and prepared runtime objects for a single FMU.
}

class Graph {
    Organizes invocable nodes and orchestrates their execution order.
}

class GraphBuilder {
    Constructs execution graphs from analysis results and recorder hooks.
}

class FmuModel {
    Wraps an FMU instance with buffers, connections, and invocation logic.
}

class AsyncNode {
    Runs an invocable object on a dedicated worker thread.
}

class Invocable {
    Interface for objects that expose init and invoke entry points.
}

class InvocableNode {
    Interface marker for graph nodes that can be scheduled.
}

class ExecutionBase {
    Abstract executor that coordinates invocation of graph nodes.
}

class Jacobi {
    Execution strategy invoking nodes in parallel-style rounds.
}

class Seidel {
    Execution strategy invoking nodes sequentially with feedback.
}

class AnalysisGraph {
    Represents FMU models, connectors, and their relationships for analysis.
}

class AnalysisGraphBuilder {
    Transforms SSP definitions into analysis graphs.
}

class DataRecorder {
    Collects and persists signal data during simulation runs.
}


class SignalStorage {
    Allocates contiguous memory areas and metadata for stored signals.
}

Simulator *-- SimulationRuntime
SimulationRuntime *-- ControlChannel
SimulationRuntime *-- PacingClock
SimulationRuntime *-- SimulatorCore
SimulatorCore *-- FmuHandler
SimulatorCore *-- DataRecorder
SimulatorCore *-- Graph
SimulatorCore *--  GraphBuilder
SimulatorCore *--  AnalysisGraphBuilder
AnalysisGraph --> GraphBuilder

FmuHandler *-- "many" FmuInfo

GraphBuilder --> Graph
InvocableNode ..> Graph
Graph *-- AsyncNode
ExecutionBase ..> Jacobi 
ExecutionBase ..> Seidel 
Seidel --> Graph 
Jacobi --> Graph 

Invocable ..> InvocableNode
InvocableNode ..> AsyncNode
Invocable ..> FmuModel
FmuInfo --> FmuModel
FmuModel --> SignalStorage
AsyncNode *-- FmuModel

AnalysisGraphBuilder --> AnalysisGraph

SignalStorage --> DataRecorder
