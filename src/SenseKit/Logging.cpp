#include "Logging.h"

#include <SenseKit/sensekit_types.h>

INITIALIZE_LOGGING

namespace sensekit {

    void initialize_logging(const char* logFilePath)
    {
        const char TRUE_STRING[] = "true";

        el::Loggers::addFlag(el::LoggingFlag::CreateLoggerAutomatically);
        el::Loggers::addFlag(el::LoggingFlag::ColoredTerminalOutput);
        el::Loggers::addFlag(el::LoggingFlag::HierarchicalLogging);

        el::Configurations defaultConf;

        defaultConf.setToDefault();
        defaultConf.setGlobally(el::ConfigurationType::Enabled, TRUE_STRING);
        defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, TRUE_STRING);
        defaultConf.setGlobally(el::ConfigurationType::ToFile, TRUE_STRING);
        defaultConf.setGlobally(el::ConfigurationType::Filename, logFilePath);

        el::Loggers::setDefaultConfigurations(defaultConf, true);
        el::Loggers::setLoggingLevel(el::Level::Debug);

        defaultConf.clear();
    }
}