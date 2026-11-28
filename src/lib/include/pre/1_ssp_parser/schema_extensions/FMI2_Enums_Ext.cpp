
#include "ssp4cpp/utils/log.hpp"

#include "FMI2_Enums_Ext.hpp"
#include "utils/fmi/data_type_traits.hpp"

#include <string>
#include <stdexcept>

namespace ssp4sim::ext::fmi2
{

    namespace enums
    {
        ssp4cpp::utils::log::Logger *log()
        {
            // Cache this logger locally so we avoid eager header initialization.
            static ssp4cpp::utils::log::Logger *logger =
                ssp4cpp::utils::log::make_logger("ssp4sim.ext.fmi2.enums");
            return logger;
        }

        /**
         * @brief  Return the in-memory size (in bytes) of a single value
         *         represented by the given DataType.
         */
        std::size_t get_data_type_size(types::DataType t)
        {
            if (utils::fmi::is_int_backed(t))
            {
                return sizeof(int); // typically 4
            }
            if (t == types::DataType::real)
            {
                return sizeof(double); // typically 8
            }
            if (utils::fmi::is_string(t))
            {
                return sizeof(std::string);
            }
            if (t == types::DataType::unknown)
            {
                return 0;
            }
            throw std::invalid_argument("Unknown DataType");
        }

        std::string data_type_to_string(types::DataType type, void *data)
        {
            IF_LOG({
                LOG_TRACE_L2(log(), "[{func}] init", __func__);
            });
            if (type == types::DataType::real)
            {
                return std::to_string(*(double *)data);
            }
            if (type == types::DataType::boolean)
            {
                return std::to_string(*(int *)data != 0 ? 1 : 0);
            }
            if (utils::fmi::is_integer_like(type))
            {
                return std::to_string(*(int *)data);
            }
            if (utils::fmi::is_string(type))
            {
                return *(std::string *)data;
            }
            return "<bin>";
        }

        DefaultValue get_default_value(types::DataType type)
        {
            if (type == types::DataType::boolean)
            {
                return false;
            }
            if (utils::fmi::is_integer_like(type))
            {
                return 0;
            }
            if (type == types::DataType::real)
            {
                return 0.0;
            }
            if (utils::fmi::is_string(type))
            {
                return "";
            }
            return 0;
        }
    }

}
