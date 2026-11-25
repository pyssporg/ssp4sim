classDiagram

class Simulator {
  +engine: std::unique_ptr<SimulationEngine>
  +run_mutex: std::mutex
  +run_thread: std::thread
  +run_future: std::shared_future<RunResult>
  +run_started: bool
  +Simulator(config)
  +init()
  +simulate()
  +start()
  +info() RunInfo
  +wait()
}

class SimulationEngine {
  +ssp: std::unique_ptr<Ssp>
  +conf: std::unique_ptr<SharedConfig>
  +session_uuid: std::string
  +sim_graph: pre::SimulationGraph
  +sim: pre::SimulationData
  +recorder: std::unique_ptr<signal::DataRecorder>
  +simulation_node: std::shared_ptr<graph::Invocable>
  +control: std::shared_ptr<control::RunControl>
  +init()
  +run(): RunResult
  +info() RunInfo
}

note for SimulationEngine "run() owns the outer macro-step loop: slices by the configured timestep, calls RunControl::before_slice() at each slice top, and publishes the last completed boundary before finalizing with RunControl::finish()"

class RunControl {
  +mutex_: std::mutex
  +cv_: std::condition_variable
  +deferred_: std::multimap
  +state_: RunState
  +sim_time_ns_: uint64_t
  +speed_: double
  +before_slice(slice_start_ns) bool
  +publish_boundary(boundary_ns)
  +request_stop()
  +finish(result) RunResult
  +info() RunInfo
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


Simulator *-- SimulationEngine
SimulationEngine *-- RunControl
SimulationEngine ..> RunInfo
Simulator ..> RunInfo
SimulationEngine *-- FmuHandler
SimulationEngine *-- DataRecorder
SimulationEngine *-- Graph
SimulationEngine *--  GraphBuilder
SimulationEngine *--  AnalysisGraphBuilder
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
