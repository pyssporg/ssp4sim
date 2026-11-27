#include "simulation/runtime/simulator_rebuild.hpp"

#include "cpp_api/simulator.hpp"

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace ssp4sim
{
    namespace detail
    {
        RebuildResult RebuildAccess::rebuild(
            Simulator *old,
            SharedConfig config,
            const std::filesystem::path *snapshot)
        {
            RebuildResult result;

            if (old)
            {
                result.old_info = old->info();
            }
            result.had_run =
                old && result.old_info.state != RunState::Created &&
                result.old_info.state != RunState::Initialized;
            const double prior_speed = old ? result.old_info.speed : 0.0;

            if (result.had_run)
            {
                result.old_run_error = old->shutdown_report();
                result.old_info = old->info();
            }

            try
            {
                if (snapshot)
                {
                    config = Simulator::prepare_restore_config(
                        std::move(config), prior_speed);
                    result.replacement =
                        Simulator::create_restored(std::move(config), *snapshot);
                }
                else
                {
                    result.replacement =
                        Simulator::create_initialized(std::move(config));
                }
            }
            catch (const std::exception &exception)
            {
                result.exception = std::current_exception();
                result.message = exception.what();
                result.replacement.reset();
            }
            catch (...)
            {
                result.exception = std::current_exception();
                result.message = "unknown error";
                result.replacement.reset();
            }

            return result;
        }
    }
}
