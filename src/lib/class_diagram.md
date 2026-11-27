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
  +snapshot(path) SnapshotSummary
  +restore(path, start)
  +reset()
  +rebuild(old, config, snapshot)$ RebuildResult
  +create_initialized(config)$ std::unique_ptr<Simulator>
  +create_restored(config, path)$ std::unique_ptr<Simulator>
}

note for Simulator "Simulator is the caller facade: it retains the creation config and delegates init/start/simulate/pause/resume/stop/set_speed/info/snapshot/wait/shutdown to its SimulationRuntime; restore and rebuild construct replacements through the private factories."

class SimulationRuntime {
  +config: SharedConfig
  +log: Logger*
  +core: std::unique_ptr<SimulatorCore>
  +control: std::unique_ptr<RunControl>
  +run_mutex: std::mutex
  +run_thread: std::thread
  +run_future: std::shared_future<RunResult>
  +run_started: bool
  +init()
  +start()
  +simulate()
  +pause(at_sim_time_ns)
  +resume()
  +stop(at_sim_time_ns)
  +set_speed(speed, at_sim_time_ns)
  +info() RunInfo
  +snapshot(path) SnapshotSummary
  +wait() RunResult
  +shutdown() std::string
  +load_snapshot(path)
  +run() RunResult
  +has_run_thread() bool
  +close_and_cancel_work()
}

note for SimulationRuntime "SimulationRuntime owns the simulation side: the synchronous core, the run thread/mutex/future, and the outer macro-step loop. RunControl is the single locked queue/status object in the same simulation/runtime/ folder. run() calls RunControl::next_action(), SimulatorCore::step(), RunControl::publish_time(), then flushes and finalizes through RunControl::finish(); every exit path shares one epilogue that closes run work once before the recorder flush. snapshot() posts one private SnapshotWork : RunThreadTask and blocks on its promise; the core owns save/load."

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
  +step(): StepStatus
  +advance_to(target): StepStatus
  +sim_time_ns() uint64_t
  +end_time_ns() uint64_t
  +save_snapshot(path) SnapshotSummary
  +load_snapshot(path)
  +finish()
}

class SnapshotSummary {
  +sim_time_ns: uint64_t
  +model_count: size_t
  +file_bytes: size_t
}

class RunControl {
  +mutex_: std::mutex
  +cv_: std::condition_variable
  +state_: RunState
  +clock_: PacingClock
  +events_: std::multimap<uint64_t, Event>
  +closed_: bool
  +next_action(step_start_ns) LoopAction
  +post_work(work) bool
  +pop_work() RunThreadTask
  +drain_work()
  +close_events()
  +pending_events() size_t
  +publish_time(time_ns)
  +request_stop()
  +finish(result) RunResult
  +info() RunInfo
}

class PacingClock {
  +sim_time_ns_: uint64_t
  +speed_: double
  +anchor_wall_ns_: int64_t
  +anchor_sim_ns_: uint64_t
  +advance_to(time_ns)
  +apply_speed(speed)
  +anchor_at(sim_time_ns)
  +compute_deadline_ns() uint64_t
}

class RebuildResult {
  +replacement: std::unique_ptr
  +exception: std::exception_ptr
  +message: std::string
  +old_info: RunInfo
  +old_run_error: std::string
  +had_run: bool
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
SimulationRuntime *-- RunControl
SimulationRuntime *-- SimulatorCore
Simulator ..> SnapshotSummary
RunControl *-- PacingClock
RunControl *-- RunThreadTask
SimulatorCore ..> SnapshotSummary
Simulator ..> RebuildResult
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
