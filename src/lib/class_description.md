classDiagram

class Simulator {
    The async runtime: owns the worker thread, command queue, pacing, RunState including Stopping, start barrier, wait, and terminal result. Drives SimulatorCore and RunControl; start() blocks until running or terminal. snapshot() posts one run-thread task and blocks; no snapshot service or correlation state lives here.
}

class SimulatorCore {
    Synchronous core: owns SSP import, graph, executors, recorder, simulation time, snapshot save/load, and one macro step; no thread, pacing, RunState, or command queue.
}

class SimulationRuntime {
    Owns the simulation side: the synchronous SimulatorCore, the run thread and future, the outer macro-step loop, and the single RunControl queue/status object; marshals snapshots onto the run thread.
}

class RunControl {
    Single shared interface between caller and run thread: one mutex and one condition variable guard the lifecycle state, PacingClock (simulation time, speed, pacing anchors, deadline math), and one time-ordered multimap of commands and run work. next_action() is the single loop gate; immediate control mutates state under the same lock, and info() is the status readback.
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
SimulationRuntime *-- RunControl
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
