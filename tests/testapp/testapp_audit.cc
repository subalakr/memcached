/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 * Tests that check certain events make it into the audit log.
 */

#include "testapp.h"

#include <platform/dirutils.h>

#include <string>
#include <fstream>

/*
 * AuditTest runs memcached with auditing enabled.
 *
 * Each test operates by stopping memcached to ensure the audit log
 * is written, thus Setup() always starts memcached.
 *
 */
class AuditTest : public TestappTest {
public:

    /*
     * 1. Check memcached isn't running (stop if it is).
     * 2. Create an audit config file for this test.
     * 3. Update memcached config file with the audit config.
     * 4. start memcached with auditing enabled.
     * 5. Connect to memcached/create bucket ready for a test.
     */
    void SetUp() {
        if (server_pid != pid_t(-1)) {
            // We need to restart with auditing enabled.
            stop_memcached_server();
        }

        memcached_cfg.reset(generate_config(/*disable SSL*/0));

        // Setup audit config file name
        char auditConfigFileName[1024];
        const char* pattern = "memcached_testapp.audit.json.XXXXXX";
        const std::string cwd = get_working_current_directory();
        snprintf(auditConfigFileName,
                 sizeof(auditConfigFileName),
                 "%s/%s",
                 cwd.c_str(),
                 pattern);

        cb_mktemp(auditConfigFileName);
        AuditTest::auditConfigFileName = auditConfigFileName;

        // Generate the auditd config file.
        unique_cJSON_ptr root(cJSON_CreateObject());
        cJSON_AddNumberToObject(root.get(), "version", 1);
        cJSON_AddTrueToObject(root.get(), "auditd_enabled");
        cJSON_AddNumberToObject(root.get(), "rotate_interval", 1440);
        cJSON_AddNumberToObject(root.get(), "rotate_size", 20971520);
        cJSON_AddFalseToObject(root.get(), "buffered");

        auditLogDirName = AuditTest::auditConfigFileName;
        auditLogDirName += ".log";

        cJSON_AddStringToObject(root.get(), "log_path", auditLogDirName.c_str());
        cJSON_AddStringToObject(root.get(), "descriptors_path", "auditd");
        cJSON_AddItemToObject(root.get(), "sync", cJSON_CreateArray());
        cJSON_AddItemToObject(root.get(), "disabled", cJSON_CreateArray());
        cb_mktemp(auditConfigFileName);
        char* configString = cJSON_Print(root.get());
        write_config_to_file(configString, auditConfigFileName);
        cJSON_Free(configString);

        cJSON_AddStringToObject(memcached_cfg.get(),
                                "audit_file",
                                auditConfigFileName);

        ASSERT_TRUE(CouchbaseDirectoryUtilities::mkdirp(auditLogDirName));

        start_memcached_server(memcached_cfg.get());

        if (HasFailure()) {
            server_pid = reinterpret_cast<pid_t>(-1);
        } else {
            CreateTestBucket();
        }

        sock = connect_to_server_plain(port);
        ASSERT_NE(INVALID_SOCKET, sock);

        // Set ewouldblock_engine test harness to default mode.
        ewouldblock_engine_configure(ENGINE_EWOULDBLOCK, EWBEngineMode::First,
                                     /*unused*/0);
        setControlToken();
    }


    void TearDown() {
        ASSERT_TRUE(CouchbaseDirectoryUtilities::rmrf(auditLogDirName));
        EXPECT_TRUE(CouchbaseDirectoryUtilities::rmrf(auditConfigFileName));
        TestappTest::TearDownTestCase();
    }

    std::vector<unique_cJSON_ptr> readAuditData();
    bool searchAuditLogForID(int id);

    static unique_cJSON_ptr memcached_cfg;
    static std::string auditConfigFileName;
    static std::string auditLogDirName;
};

unique_cJSON_ptr AuditTest::memcached_cfg;
std::string AuditTest::auditConfigFileName;
std::string AuditTest::auditLogDirName;

std::vector<unique_cJSON_ptr> AuditTest::readAuditData() {
    std::vector<unique_cJSON_ptr> rval;
    auto files = CouchbaseDirectoryUtilities::findFilesContaining(auditLogDirName,
                                                                  "audit.log");
    for (auto file : files ) {
        std::ifstream auditData(file, std::ifstream::in);
        while(auditData.good()) {
            std::string line;
            std::getline(auditData, line);
            unique_cJSON_ptr jsonPtr(cJSON_Parse(line.c_str()));
            if (jsonPtr.get() != nullptr) {
               rval.push_back(std::move(jsonPtr));
            }
        }
    }
    return rval;
}

bool AuditTest::searchAuditLogForID(int id) {
    auto auditEntries = readAuditData();
    for (auto& entry : auditEntries) {
        cJSON* idEntry = cJSON_GetObjectItem(entry.get(), "id");
        if (idEntry && idEntry->valueint == id) {
            return true;
        }
    }
    return false;
}

/**
 * Validate that a rejected illegal packet is audit logged.
 */
TEST_F(AuditTest, AuditIllegalPacket) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } send, receive;
    uint64_t value = 0xdeadbeefdeadcafe;
    std::string key("AuditTest::AuditIllegalPacket");
    size_t len = mcbp_storage_command(send.bytes, sizeof(send.bytes),
                                      PROTOCOL_BINARY_CMD_SET,
                                      key.c_str(), key.size(),
                                      &value, sizeof(value),
                                      0, 0);

    // Now make packet illegal.
    auto diff = ntohl(send.request.message.header.request.bodylen) - 1;
    send.request.message.header.request.bodylen = htonl(1);
    safe_send(send.bytes, len - diff, false);

    safe_recv_packet(receive.bytes, sizeof(receive.bytes));
    mcbp_validate_response_header(&receive.response, PROTOCOL_BINARY_CMD_SET,
                                  PROTOCOL_BINARY_RESPONSE_EINVAL);

    // An invalid packet causes memcached to disconnect from the client.
    // So need to perform a reconnect to send the shutdown command.
    reconnect_to_server();
    // stop memcached so it writes out the audit logs.
    sendShutdown(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    waitForShutdown();
    ASSERT_TRUE(searchAuditLogForID(20483));
}


/**
 * Validate that we log start/stop
 */
TEST_F(AuditTest, AuditStartedStopped) {
    sendShutdown(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    waitForShutdown();
    ASSERT_TRUE(searchAuditLogForID(4096));
    ASSERT_TRUE(searchAuditLogForID(4097));
}

/**
 * Validate that a failed SASL auth is audit logged.
 */
TEST_F(AuditTest, AuditFailedAuth) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    // use a plain auth with an unknown user, easy failure to force.
    const char* chosenmech = "PLAIN";
    const char* data = "\0nouser\0nopassword";

    size_t plen = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                   PROTOCOL_BINARY_CMD_SASL_AUTH,
                                   chosenmech, strlen(chosenmech),
                                   data, sizeof(data));

    safe_send(buffer.bytes, plen, false);
    safe_recv_packet(&buffer, sizeof(buffer));
    mcbp_validate_response_header(&buffer.response, PROTOCOL_BINARY_CMD_SASL_AUTH,
                                  PROTOCOL_BINARY_RESPONSE_AUTH_ERROR);
    sendShutdown(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    waitForShutdown();
    ASSERT_TRUE(searchAuditLogForID(20481));
}

/**
 * Validate that a RBAC violation is logged.
 */
TEST_F(AuditTest, AuditRBACFailed) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    /* assume the statistics role */
    auto len = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                PROTOCOL_BINARY_CMD_ASSUME_ROLE,
                                "statistics", 10, NULL, 0);

    safe_send(buffer.bytes, len, false);
    safe_recv_packet(buffer.bytes, sizeof(buffer.bytes));
    mcbp_validate_response_header(&buffer.response, PROTOCOL_BINARY_CMD_ASSUME_ROLE,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);

    /* At this point I should get an EACCESS if I tried to run NOOP */
    len = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                           PROTOCOL_BINARY_CMD_NOOP,
                           NULL, 0, NULL, 0);

    safe_send(buffer.bytes, len, false);
    safe_recv_packet(buffer.bytes, sizeof(buffer.bytes));
    mcbp_validate_response_header(&buffer.response, PROTOCOL_BINARY_CMD_NOOP,
                                  PROTOCOL_BINARY_RESPONSE_EACCESS);
    // An RBAC violation causes memcached to disconnect from the client.
    // So need to perform a reconnect to send the shutdown command.
    reconnect_to_server();
    sendShutdown(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    waitForShutdown();
    ASSERT_TRUE(searchAuditLogForID(20484));
}