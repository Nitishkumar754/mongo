/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include <fstream>
#include <iostream>
#include <memory>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/bson/json.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sdam/sdam_json_test_runner_cli_options_gen.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

/**
 * This program runs the Server Discover and Monitoring JSON test files located in
 * the src/monogo/client/sdam/json_tests sub-directory.
 *
 * The process return code conforms to the UNIX idiom of 0 to indicate success and non-zero to
 * indicate failure. In the case of test failure, the process will return the number of test cases
 * that failed.
 *
 * Example invocation to run all tests:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests
 *
 * Example invocation to run a single test:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests --filter normalize_uri_case
 */

namespace fs = boost::filesystem;
namespace moe = mongo::optionenvironment;
using namespace mongo::sdam;

namespace mongo::sdam {

std::string emphasize(const std::string text) {
    std::stringstream output;
    const auto border = std::string(3, '#');
    output << border << " " << text << " " << border << std::endl;
    return output.str();
}

class ArgParser {
public:
    ArgParser(int argc, char* argv[]) {
        moe::OptionsParser parser;
        moe::Environment environment;
        moe::OptionSection options;

        Status ret = addCliOptions(&options);
        if (!ret.isOK()) {
            std::cerr << "Unexpected error adding cli options: " << ret.toString() << std::endl;
            MONGO_UNREACHABLE;
        }

        ret = parser.run(options, toStringVector(argc, argv), {}, &environment);
        if (argc <= 1 || !ret.isOK() || environment.count("help")) {
            if (!ret.isOK()) {
                std::cerr << "An error occurred: " << ret.toString() << std::endl;
            }
            printHelpAndExit(argv[0], options.helpString());
        }

        const auto exitIfError = [](Status status) {
            if (!status.isOK()) {
                std::cerr << "An error occurred: " << status.toString() << std::endl;
                std::exit(kArgParseExitCode);
            }
        };

        if (environment.count(kSourceDirOption)) {
            ret = environment.get(kSourceDirOption, &_sourceDirectory);
            exitIfError(ret);
        }

        if (environment.count(moe::Key(kFilterOption))) {
            ret = environment.get(moe::Key(kFilterOption), &_testFilters);
            exitIfError(ret);
        }

        if (environment.count(moe::Key(kVerbose))) {
            std::string value;
            ret = environment.get(moe::Key(kVerbose), &value);
            if (!ret.isOK())
                exitIfError(ret);
            _verbose = value.size() + 1;
        }
    }

    void LogParams() const {
        LOGV2(20199, "Verbosity: {verbose}", "verbose"_attr = _verbose);
        LOGV2(20200,
              "Source Directory: {sourceDirectory}",
              "sourceDirectory"_attr = _sourceDirectory);
        if (_testFilters.size()) {
            LOGV2(20201,
                  "Filters: {boost_join_testFilters}",
                  "boost_join_testFilters"_attr = boost::join(_testFilters, ", "));
        }
    }

    const std::string& SourceDirectory() const {
        return _sourceDirectory;
    }

    const std::vector<std::string>& TestFilters() const {
        return _testFilters;
    }

    int Verbose() const {
        return _verbose;
    }

private:
    constexpr static auto kSourceDirOption = "source-dir";
    constexpr static auto kSourceDirDefault = ".";

    constexpr static auto kFilterOption = "filter";

    constexpr static int kHelpExitCode = 0;
    constexpr static int kArgParseExitCode = 1024;

    constexpr static auto kVerbose = "verbose";

    std::string _sourceDirectory = kSourceDirDefault;
    std::vector<std::string> _testFilters;
    int _verbose = 0;

    void printHelpAndExit(char* programName, const std::string desc) {
        std::cout << programName << ":" << std::endl << desc << std::endl;
        std::exit(kHelpExitCode);
    }

    std::vector<std::string> toStringVector(int n, char** array) {
        std::vector<std::string> result;
        for (int i = 0; i < n; ++i)
            result.push_back(array[i]);
        return result;
    }
};

/**
 * This class is responsible for parsing and executing a single 'phase' of the json test
 */
class TestCasePhase {
public:
    TestCasePhase(int phaseNum, MongoURI uri, BSONObj phase) : _testUri(uri), _phaseNum(phaseNum) {
        auto bsonResponses = phase.getField("responses").Array();
        for (auto& response : bsonResponses) {
            const auto pair = response.Array();
            const auto address = pair[0].String();
            const auto bsonIsMaster = pair[1].Obj();

            if (bsonIsMaster.nFields() == 0) {
                _isMasterResponses.push_back(IsMasterOutcome(address, BSONObj(), "network error"));
            } else {
                _isMasterResponses.push_back(IsMasterOutcome(address, bsonIsMaster, kLatency));
            }
        }
        _topologyOutcome = phase["outcome"].Obj();
    }

    // pair of error subject & error description
    using TestPhaseError = std::pair<std::string, std::string>;
    struct PhaseResult {
        std::vector<TestPhaseError> errorDescriptions;
        int phaseNumber;

        bool Success() const {
            return errorDescriptions.size() == 0;
        }
    };
    using PhaseResultPtr = PhaseResult*;

    PhaseResult execute(TopologyManager& topology) const {
        PhaseResult testResult{{}, _phaseNum};

        for (auto response : _isMasterResponses) {
            auto descriptionStr =
                (response.getResponse()) ? response.getResponse()->toString() : "[ Network Error ]";
            LOGV2(20202,
                  "Sending server description: {response_getServer} : {descriptionStr}",
                  "response_getServer"_attr = response.getServer(),
                  "descriptionStr"_attr = descriptionStr);
            topology.onServerDescription(response);
        }

        LOGV2(20203,
              "TopologyDescription after Phase {phaseNum}: {topology_getTopologyDescription}",
              "phaseNum"_attr = _phaseNum,
              "topology_getTopologyDescription"_attr =
                  topology.getTopologyDescription()->toString());

        validateServers(
            &testResult, topology.getTopologyDescription(), _topologyOutcome["servers"].Obj());
        validateTopologyDescription(
            &testResult, topology.getTopologyDescription(), _topologyOutcome);

        return testResult;
    }

    int getPhaseNum() const {
        return _phaseNum;
    }

private:
    template <typename T, typename U>
    std::string errorMessageNotEqual(T expected, U actual) const {
        std::stringstream errorMessage;
        errorMessage << "expected '" << actual << "' to equal '" << expected << "'";
        return errorMessage.str();
    }

    std::string serverDescriptionFieldName(const ServerDescriptionPtr serverDescription,
                                           std::string field) const {
        std::stringstream name;
        name << "(" << serverDescription->getAddress() << ") " << field;
        return name.str();
    }

    std::string topologyDescriptionFieldName(std::string field) const {
        std::stringstream name;
        name << "(topologyDescription) " << field;
        return name.str();
    }

    template <typename EVO, typename AV>
    void doValidateServerField(const PhaseResultPtr result,
                               const ServerDescriptionPtr serverDescription,
                               const std::string fieldName,
                               EVO expectedValueObtainer,
                               const AV& actualValue) const {
        const auto expectedValue = expectedValueObtainer();
        if (expectedValue != actualValue) {
            auto errorDescription =
                std::make_pair(serverDescriptionFieldName(serverDescription, fieldName),
                               errorMessageNotEqual(expectedValue, actualValue));
            result->errorDescriptions.push_back(errorDescription);
        }
    }

    void validateServerField(const PhaseResultPtr result,
                             const ServerDescriptionPtr& serverDescription,
                             const BSONElement& expectedField) const {
        const auto serverAddress = serverDescription->getAddress();

        std::string fieldName = expectedField.fieldName();
        if (fieldName == "type") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      auto status = parseServerType(expectedField.String());
                                      if (!status.isOK()) {
                                          auto errorDescription = std::make_pair(
                                              serverDescriptionFieldName(serverDescription, "type"),
                                              status.getStatus().toString());
                                          result->errorDescriptions.push_back(errorDescription);

                                          // return the actual value since we already have reported
                                          // an error about the parsed server type from the json
                                          // file.
                                          return serverDescription->getType();
                                      }
                                      return status.getValue();
                                  },
                                  serverDescription->getType());

        } else if (fieldName == "setName") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<std::string> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.String();
                                      }
                                      return result;
                                  },
                                  serverDescription->getSetName());

        } else if (fieldName == "setVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<int> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.numberInt();
                                      }
                                      return result;
                                  },
                                  serverDescription->getSetVersion());

        } else if (fieldName == "electionId") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<OID> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.OID();
                                      }
                                      return result;
                                  },
                                  serverDescription->getElectionId());

        } else if (fieldName == "logicalSessionTimeoutMinutes") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<int> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.numberInt();
                                      }
                                      return result;
                                  },
                                  serverDescription->getLogicalSessionTimeoutMinutes());

        } else if (fieldName == "minWireVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() { return expectedField.numberInt(); },
                                  serverDescription->getMinWireVersion());

        } else if (fieldName == "maxWireVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() { return expectedField.numberInt(); },
                                  serverDescription->getMaxWireVersion());

        } else {
            MONGO_UNREACHABLE;
        }
    }

    void validateServers(PhaseResultPtr result,
                         const TopologyDescriptionPtr topologyDescription,
                         const BSONObj bsonServers) const {
        auto actualNumServers = topologyDescription->getServers().size();
        auto expectedNumServers =
            bsonServers.getFieldNames<stdx::unordered_set<std::string>>().size();

        if (actualNumServers != expectedNumServers) {
            std::stringstream errorMessage;
            errorMessage << "expected " << expectedNumServers
                         << " server(s) in topology description. actual was " << actualNumServers
                         << ": ";
            for (const auto& server : topologyDescription->getServers()) {
                errorMessage << server->getAddress() << ", ";
            }
            result->errorDescriptions.push_back(std::make_pair("servers", errorMessage.str()));
        }

        for (const BSONElement& bsonExpectedServer : bsonServers) {
            const auto& serverAddress = bsonExpectedServer.fieldName();
            const auto& expectedServerDescriptionFields = bsonExpectedServer.Obj();

            const auto& serverDescription = topologyDescription->findServerByAddress(serverAddress);
            if (serverDescription) {
                for (const BSONElement& field : expectedServerDescriptionFields) {
                    validateServerField(result, *serverDescription, field);
                }
            } else {
                std::stringstream errorMessage;
                errorMessage << "could not find server '" << serverAddress
                             << "' in topology description.";
                auto errorDescription = std::make_pair("servers", errorMessage.str());
                result->errorDescriptions.push_back(errorDescription);
            }
        }
    }

    template <typename EVO, typename AV>
    void doValidateTopologyDescriptionField(const PhaseResultPtr result,
                                            const std::string fieldName,
                                            EVO expectedValueObtainer,
                                            const AV& actualValue) const {
        auto expectedValue = expectedValueObtainer();
        if (expectedValue != actualValue) {
            auto errorDescription =
                std::make_pair(topologyDescriptionFieldName(fieldName),
                               errorMessageNotEqual(expectedValue, actualValue));
            result->errorDescriptions.push_back(errorDescription);
        }
    }

    void validateTopologyDescription(PhaseResultPtr result,
                                     const TopologyDescriptionPtr topologyDescription,
                                     const BSONObj bsonTopologyDescription) const {
        {
            constexpr auto fieldName = "topologyType";
            doValidateTopologyDescriptionField(
                result,
                fieldName,
                [&]() { return bsonTopologyDescription[fieldName].String(); },
                toString(topologyDescription->getType()));
        }

        {
            constexpr auto fieldName = "setName";
            doValidateTopologyDescriptionField(result,
                                               fieldName,
                                               [&]() {
                                                   boost::optional<std::string> ret;
                                                   auto bsonField =
                                                       bsonTopologyDescription[fieldName];
                                                   if (!bsonField.isNull()) {
                                                       ret = bsonField.String();
                                                   }
                                                   return ret;
                                               },
                                               topologyDescription->getSetName());
        }

        {
            constexpr auto fieldName = "logicalSessionTimeoutMinutes";
            doValidateTopologyDescriptionField(
                result,
                fieldName,
                [&]() {
                    boost::optional<int> ret;
                    auto bsonField = bsonTopologyDescription[fieldName];
                    if (!bsonField.isNull()) {
                        ret = bsonField.numberInt();
                    }
                    return ret;
                },
                topologyDescription->getLogicalSessionTimeoutMinutes());
        }

        {
            constexpr auto fieldName = "maxSetVersion";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(result,
                                                   fieldName,
                                                   [&]() {
                                                       boost::optional<int> ret;
                                                       auto bsonField =
                                                           bsonTopologyDescription[fieldName];
                                                       if (!bsonField.isNull()) {
                                                           ret = bsonField.numberInt();
                                                       }
                                                       return ret;
                                                   },
                                                   topologyDescription->getMaxSetVersion());
            }
        }

        {
            constexpr auto fieldName = "maxElectionId";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(result,
                                                   fieldName,
                                                   [&]() {
                                                       boost::optional<OID> ret;
                                                       auto bsonField =
                                                           bsonTopologyDescription[fieldName];
                                                       if (!bsonField.isNull()) {
                                                           ret = bsonField.OID();
                                                       }
                                                       return ret;
                                                   },
                                                   topologyDescription->getMaxElectionId());
            }
        }

        {
            constexpr auto fieldName = "compatible";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(
                    result,
                    fieldName,
                    [&]() { return bsonTopologyDescription[fieldName].Bool(); },
                    topologyDescription->isWireVersionCompatible());
            }
        }
    }

    // the json tests don't actually use this value.
    constexpr static auto kLatency = mongo::Milliseconds(100);

    MongoURI _testUri;
    int _phaseNum;
    std::vector<IsMasterOutcome> _isMasterResponses;
    BSONObj _topologyOutcome;
};

/**
 * This class is responsible for parsing and executing a single json test file.
 */
class JsonTestCase {
public:
    JsonTestCase(fs::path testFilePath) {
        parseTest(testFilePath);
    }

    struct TestCaseResult {
        std::vector<TestCasePhase::PhaseResult> phaseResults;
        std::string file;
        std::string name;

        bool Success() const {
            return std::all_of(
                phaseResults.begin(),
                phaseResults.end(),
                [](const TestCasePhase::PhaseResult& result) { return result.Success(); });
        }
    };

    TestCaseResult execute() {
        auto config =
            std::make_unique<SdamConfiguration>(getSeedList(),
                                                _initialType,
                                                SdamConfiguration::kDefaultHeartbeatFrequencyMs,
                                                _replicaSetName);

        auto clockSource = std::make_unique<ClockSourceMock>();
        TopologyManager topology(*config, clockSource.get());

        TestCaseResult result{{}, _testFilePath, _testName};

        for (const auto& testPhase : _testPhases) {
            LOGV2(20204,
                  "{emphasize_Phase_std_to_string_testPhase_getPhaseNum}",
                  "emphasize_Phase_std_to_string_testPhase_getPhaseNum"_attr =
                      emphasize("Phase " + std::to_string(testPhase.getPhaseNum())));
            auto phaseResult = testPhase.execute(topology);
            result.phaseResults.push_back(phaseResult);
            if (!result.Success()) {
                LOGV2(20205,
                      "Phase {phaseResult_phaseNumber} failed.",
                      "phaseResult_phaseNumber"_attr = phaseResult.phaseNumber);
                break;
            }
        }

        return result;
    }

    const std::string& Name() const {
        return _testName;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(20206, "");
        LOGV2(20207,
              "{emphasize_Parsing_testFilePath_string}",
              "emphasize_Parsing_testFilePath_string"_attr =
                  emphasize("Parsing " + testFilePath.string()));
        {
            std::ifstream testFile(_testFilePath);
            std::ostringstream json;
            json << testFile.rdbuf();
            _jsonTest = fromjson(json.str());
        }

        _testName = _jsonTest.getStringField("description");
        _testUri = uassertStatusOK(mongo::MongoURI::parse(_jsonTest["uri"].String()));

        _replicaSetName = _testUri.getOption("replicaSet");
        if (!_replicaSetName) {
            if (_testUri.getServers().size() == 1) {
                _initialType = TopologyType::kSingle;
            } else {
                // We can technically choose either kUnknown or kSharded and be compliant,
                // but it seems that some of the json tests assume kUnknown as the initial state.
                // see: json_tests/sharded/normalize_uri_case.json
                _initialType = TopologyType::kUnknown;
            }
        } else {
            _initialType = TopologyType::kReplicaSetNoPrimary;
        }

        int phase = 0;
        const std::vector<BSONElement>& bsonPhases = _jsonTest["phases"].Array();
        for (auto bsonPhase : bsonPhases) {
            _testPhases.push_back(TestCasePhase(phase++, _testUri, bsonPhase.Obj()));
        }
    }

    std::vector<ServerAddress> getSeedList() {
        std::vector<ServerAddress> result;
        for (const auto& hostAndPort : _testUri.getServers()) {
            result.push_back(hostAndPort.toString());
        }
        return result;
    }

    BSONObj _jsonTest;
    std::string _testName;
    MongoURI _testUri;
    std::string _testFilePath;
    TopologyType _initialType;
    boost::optional<std::string> _replicaSetName;
    std::vector<TestCasePhase> _testPhases;
};

/**
 * This class runs (potentially) multiple json tests and reports their results.
 */
class SdamJsonTestRunner {
public:
    SdamJsonTestRunner(std::string testDirectory, std::vector<std::string> testFilters)
        : _testFiles(scanTestFiles(testDirectory, testFilters)) {}

    std::vector<JsonTestCase::TestCaseResult> runTests() {
        std::vector<JsonTestCase::TestCaseResult> results;
        const auto testFiles = getTestFiles();
        for (auto jsonTest : testFiles) {
            auto testCase = JsonTestCase(jsonTest);
            try {
                LOGV2(20208,
                      "{emphasize_Executing_testCase_Name}",
                      "emphasize_Executing_testCase_Name"_attr =
                          emphasize("Executing " + testCase.Name()));
                results.push_back(testCase.execute());
            } catch (const DBException& ex) {
                std::stringstream error;
                error << "Exception while executing " << jsonTest.string() << ": " << ex.toString();
                std::string errorStr = error.str();
                results.push_back(JsonTestCase::TestCaseResult{
                    {TestCasePhase::PhaseResult{{std::make_pair("exception", errorStr)}, 0}},
                    jsonTest.string(),
                    testCase.Name()});
                std::cerr << errorStr;
            }
        }
        return results;
    }

    int report(std::vector<JsonTestCase::TestCaseResult> results) {
        int numTestCases = results.size();
        int numSuccess = 0;
        int numFailed = 0;

        if (std::any_of(
                results.begin(), results.end(), [](const JsonTestCase::TestCaseResult& result) {
                    return !result.Success();
                })) {
            LOGV2(20209,
                  "{emphasize_Failed_Test_Results}",
                  "emphasize_Failed_Test_Results"_attr = emphasize("Failed Test Results"));
        }

        for (const auto result : results) {
            auto file = result.file;
            auto testName = result.name;
            auto phaseResults = result.phaseResults;
            if (result.Success()) {
                ++numSuccess;
            } else {
                LOGV2(
                    20210, "{emphasize_testName}", "emphasize_testName"_attr = emphasize(testName));
                LOGV2(20211, "error in file: {file}", "file"_attr = file);
                ++numFailed;
                for (auto phaseResult : phaseResults) {
                    LOGV2(20212,
                          "Phase {phaseResult_phaseNumber}: ",
                          "phaseResult_phaseNumber"_attr = phaseResult.phaseNumber);
                    if (!phaseResult.Success()) {
                        for (auto error : phaseResult.errorDescriptions) {
                            LOGV2(20213,
                                  "\t{error_first}: {error_second}",
                                  "error_first"_attr = error.first,
                                  "error_second"_attr = error.second);
                        }
                    }
                }
                LOGV2(20214, "");
            }
        }
        LOGV2(20215,
              "{numTestCases} test cases; {numSuccess} success; {numFailed} failed.",
              "numTestCases"_attr = numTestCases,
              "numSuccess"_attr = numSuccess,
              "numFailed"_attr = numFailed);

        return numFailed;
    }

    const std::vector<fs::path>& getTestFiles() const {
        return _testFiles;
    }

private:
    std::vector<fs::path> scanTestFiles(std::string testDirectory,
                                        std::vector<std::string> filters) {
        std::vector<fs::path> results;
        for (const auto& entry : fs::recursive_directory_iterator(testDirectory)) {
            if (!fs::is_directory(entry) && matchesFilter(entry, filters)) {
                results.push_back(entry.path());
            }
        }
        return results;
    }

    bool matchesFilter(const fs::directory_entry& entry, std::vector<std::string> filters) {
        const auto filePath = entry.path();
        if (filePath.extension() != ".json") {
            return false;
        }

        if (filters.size() == 0) {
            return true;
        }

        for (const auto& filter : filters) {
            if (filePath.string().find(filter) != std::string::npos) {
                return true;
            } else {
                LOGV2_DEBUG(20216,
                            2,
                            "'{filePath_string}' skipped due to filter configuration.",
                            "filePath_string"_attr = filePath.string());
            }
        }

        return false;
    }

    std::vector<fs::path> _testFiles;
};
};  // namespace mongo::sdam

int main(int argc, char* argv[]) {
    ArgParser args(argc, argv);

    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Debug(args.Verbose()));
    args.LogParams();

    SdamJsonTestRunner testRunner(args.SourceDirectory(), args.TestFilters());
    return testRunner.report(testRunner.runTests());
}
