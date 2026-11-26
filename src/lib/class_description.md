classDiagram

class Simulator {
    Public compatibility facade over ThreadedRunner: owns setup, control, and query access; start() blocks until running or terminal.
}

class ThreadedRunner {
    Owns the only async layer: worker thread, command queue, pacing, RunState including Stopping, start barrier, wait, and terminal result. Drives SimulationStepper and RunControl.
}

class SimulationStepper {
    Synchronous core: owns SSP import, graph, executors, recorder, simulation time, and one macro step; no thread, pacing, RunState, or command queue.
}

class RunControl {
    Single owner of the run-control state machine: one mutex guards the plain state, time, speed, and pacing fields, and one ordered command queue.
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

Simulator *-- ThreadedRunner
ThreadedRunner *-- RunControl
ThreadedRunner *-- SimulationStepper
SimulationStepper *-- FmuHandler
SimulationStepper *-- DataRecorder
SimulationStepper *-- Graph
SimulationStepper *--  GraphBuilder
SimulationStepper *--  AnalysisGraphBuilder
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
