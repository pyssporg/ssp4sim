classDiagram

class Simulator {
  +config: SharedConfig
  +Simulator(config)
  +init()
  +simulate()
  +start()
  +pause(at_sim_time_ns)
  +resume()
  +stop(at_sim_time_ns)
  +set_speed(speed, at_sim_time_ns)
  +info() RunInfo
  +wait() RunResult
  +shutdown()
  +snapshot(path) bool
  +restore(path, start)
  +reset()
  +create_initialized(config)$ std::unique_ptr<Simulator>
  +create_restored(config, path)$ std::unique_ptr<Simulator>
  -submit_and_wait(action)
  -shutdown_report() std::string
}

note for Simulator "Simulator is the caller facade and sole action producer: it retains the creation config, posts Start/Pause/Resume/Stop/SetSpeed/Snapshot through its SimulationRuntime's ControlChannel with post/post_and_wait, and reads status through it. The runtime only applies actions and publishes state. restore/reset construct replacements through the private factories via detail::RebuildAccess."

class SimulationRuntime {
  +config: SharedConfig
  +log: Logger*
  +core: std::unique_ptr<SimulatorCore>
  +channel: std::unique_ptr<ControlChannel>
  +clock: PacingClock
  +lifecycle_mutex: std::mutex
  +run_thread: std::thread
  +run_future: std::shared_future<RunResult>
  +run_started: bool
  +init()
  +start_thread()
  +wait() RunResult
  +join() std::string
  +load_snapshot(path)
  +control() ControlChannel&
  +started() bool
  +set_speed_before_start(speed)
  +cancel_before_start()
  +apply(action) bool
  +next_pacing_deadline()
  +run() RunResult
}

note for SimulationRuntime "SimulationRuntime owns the synchronous core, the run thread/future guarded by a separate lifecycle mutex, and the outer macro-step loop, and it never posts actions. ControlChannel is the single locked queue/status object: one mutex, one condition variable, one multimap<due_ns, Action> and the published ChannelStatus; only the runtime writes RunState. run() takes a due Action, steps when Running, otherwise waits on the channel condition variable; the epilogue closes the channel, publishes the final time, flushes the recorder, then publishes the terminal state."

class SimulatorCore {
  +ssp: std::unique_ptr<Ssp>
  +conf: std::unique_ptr<SharedConfig>
  +session_uuid: std::string
  +sim_graph: pre::SimulationGraph
  +sim: pre::SimulationData
  +recorder: std::unique_ptr<signal::DataRecorder>
  +simulation_node: std::shared_ptr<graph::Invocable>
  +init()
  +start()
  +step(): bool
  +advance_to(target): bool
  +sim_time_ns() uint64_t
  +end_time_ns() uint64_t
  +save_snapshot(path) bool
  +load_snapshot(path)
  +finish()
}

class Action {
  +seq: uint64_t
  +kind: ActionKind
  +at_ns: uint64_t
  +speed: double
  +path: std::string
  +status: ActionStatus
}

class ChannelStatus {
  +state: RunState
  +sim_time_ns: uint64_t
  +speed: double
  +applied_seq: uint64_t
  +error: std::exception_ptr
}

class ControlChannel {
  +mutex_: std::mutex
  +cv_: std::condition_variable
  +state_: RunState
  +sim_time_ns_: uint64_t
  +speed_: double
  +applied_seq_: uint64_t
  +queue_: std::multimap<uint64_t, Action>
  +closed_: bool
  +post(action) uint64_t
  +post_and_wait(action) AppliedAction
  +status() ChannelStatus
  +pending() size_t
  +take_due(sim_time_ns) optional<Action>
  +wait_for_work(sim_time_ns, deadline)
  +publish_time(sim_time_ns)
  +set_speed(speed)
  +set_state(state)
  +publish_error(error)
  +settle(seq, status, error)
  +close(error)
  +wait_terminal() RunState
}

class PacingClock {
  +speed_: double
  +anchor_wall_ns_: int64_t
  +anchor_sim_ns_: uint64_t
  +speed() double
  +anchor_wall_ns() int64_t
  +anchor_sim_ns() uint64_t
  +apply_speed(speed, sim_time_ns)
  +anchor_at(sim_time_ns)
  +deadline_ns(...)$ uint64_t
}

class RebuildResult {
  +replacement: std::unique_ptr
  +exception: std::exception_ptr
  +message: std::string
  +old_info: RunInfo
  +old_run_error: std::string
  +had_run: bool
}

class RebuildAccess {
  +rebuild(old, config, snapshot)$ RebuildResult
}

class RunInfo {
  +state: RunState
  +sim_time_ns: uint64_t
  +speed: double
}


  class FmuHandler {
    +ssp: Ssp*
    +fmu_map: std::map<string, std::unique_ptr<Fmu>>
    +fmu_info_map: std::map<string, std::unique_ptr<FmuInfo>>
    +FmuHandler(ssp)
    +init()
  }

  class FmuInfo {
    +system_name: std::string
    +model_name: std::string
    +fmu: ssp4cpp::Fmu*
    +model_description: ssp4cpp::fmi2::md::fmi2ModelDescription*
    +fmi_instance: std::unique_ptr<handler::FmuInstance>
    +model: std::unique_ptr<handler::CoSimulationModel>
    +FmuInfo(name, fmu)
  }



  class Graph {
    +models: std::map<string, std::unique_ptr<InvocableNode>>
    +nodes: std::vector<InvocableNode*>
    +executor: std::unique_ptr<ExecutionBase>
    +Graph(models)
    +init()
    +invoke(step_data)
  }

  class GraphBuilder {
    +analysis_graph: analysis::graph::AnalysisGraph*
    +recorder: signal::DataRecorder*
    +GraphBuilder(ag, recorder)
    +build(): std::unique_ptr<Graph>
  }

  class FmuModel {
    +name: std::string
    +fmu: handler::FmuInfo*
    +input_area: std::unique_ptr<signal::SignalStorage>
    +output_area: std::unique_ptr<signal::SignalStorage>
    +recorder: signal::DataRecorder*
    +connections: std::vector<ConnectionInfo>
    +FmuModel(name, fmu)
    +init()
    +invoke(step_data)
  }

  class AsyncNode {
    +worker: std::thread
    +invocable_obj: std::unique_ptr<Invocable>
    +AsyncNode(name, m)
    +init()
    +invoke(step_data)
  }

  class Invocable {
    <<Interface>>
    +init()
    +invoke(data)
  }

  class InvocableNode {
    <<Interface>>
  }


    class ExecutionBase {
      <<Abstract>>
      +models: std::vector<InvocableNode*>
      +ExecutionBase(models)
      +init()
      +invoke(step_data)
    }

    class Jacobi {
      +Jacobi(models)
      +invoke(step_data)
    }

    class Seidel {
      +Seidel(models)
      +invoke(step_data)
    }

    class AnalysisGraph {
        +models: std::map<string, std::unique_ptr<SspModel>>
        +connectors: std::map<string, std::unique_ptr<SspConnector>>
        +connections: std::map<string, std::unique_ptr<Connection>>
        +AnalysisGraph(models, connectors, connections)
    }

    class AnalysisGraphBuilder {
        +ssp: ssp4cpp::Ssp*
        +fmu_handler: handler::FmuHandler*
        +AnalysisGraphBuilder(ssp, fmu_handler)
        +build(): std::unique_ptr<AnalysisGraph>
    }


  class DataRecorder {
    +file: std::ofstream
    +trackers: std::vector<Tracker>
    +DataRecorder(filename)
    +add_storage(storage)
    +start_recording()
    +stop_recording()
  }

  class SignalStorage {
    +data: std::unique_ptr<SignalStorage>
    +capacity: size_t
    +SignalStorage(capacity, name)
    +add(name, type)
    +push(time)
  }

  class SignalStorage {
    +data: std::unique_ptr<std::byte[]>
    +positions: std::vector<std::size_t>
    +types: std::vector<types::DataType>
    +names: std::vector<std::string>
    +SignalStorage(areas)
    +add(name, type)
    +allocate()
  }


Simulator *-- SimulationRuntime
SimulationRuntime *-- ControlChannel
SimulationRuntime *-- PacingClock
SimulationRuntime *-- SimulatorCore
ControlChannel ..> Action
ControlChannel ..> ChannelStatus
Simulator ..> RebuildAccess
RebuildAccess ..> RebuildResult
Simulator ..> RunInfo
SimulationRuntime ..> RunInfo
SimulatorCore *-- FmuHandler
SimulatorCore *-- DataRecorder
SimulatorCore *-- Graph
SimulatorCore *-- GraphBuilder
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
SignalStorage *-- SignalStorage
