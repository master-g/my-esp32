/* Host tests for device_protocol, run by `make test-host`. Plain asserts, no
 * framework. argv[1] is the protocol/fixtures directory shared with the Rust
 * host agent's tests. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_protocol.h"

static cJSON *load_fixture(const char *dir, const char *name)
{
    char path[512];
    char *text = NULL;
    long size = 0;
    FILE *file = NULL;
    cJSON *root = NULL;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "rb");
    assert(file != NULL);
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = calloc(1, (size_t)size + 1);
    assert(text != NULL);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    root = cJSON_Parse(text);
    free(text);
    assert(root != NULL);
    return root;
}

static device_protocol_line_event_t feed_text(device_protocol_line_t *assembler, const char *text,
                                              size_t *dropped)
{
    device_protocol_line_event_t last = DEVICE_PROTOCOL_LINE_NONE;

    for (; *text != '\0'; ++text) {
        last = device_protocol_line_feed(assembler, *text, dropped);
    }
    return last;
}

static void test_line_assembly(void)
{
    device_protocol_line_t assembler = {0};
    size_t dropped = 0;

    /* A line is delivered at its newline; CR is ignored, blank lines are skipped. */
    assert(feed_text(&assembler, "@esp32dash {}", &dropped) == DEVICE_PROTOCOL_LINE_NONE);
    assert(feed_text(&assembler, "\r\n", &dropped) == DEVICE_PROTOCOL_LINE_READY);
    assert(strcmp(assembler.line, "@esp32dash {}") == 0);
    assert(feed_text(&assembler, "\n", &dropped) == DEVICE_PROTOCOL_LINE_NONE);
    assert(feed_text(&assembler, "next\n", &dropped) == DEVICE_PROTOCOL_LINE_READY);
    assert(strcmp(assembler.line, "next") == 0);
}

static void test_line_overflow_guard(void)
{
    device_protocol_line_t assembler = {0};
    size_t dropped = 0;
    size_t i = 0;

    /* The longest line that still fits. */
    for (i = 0; i < DEVICE_PROTOCOL_LINE_MAX - 1; ++i) {
        assert(device_protocol_line_feed(&assembler, 'a', &dropped) == DEVICE_PROTOCOL_LINE_NONE);
    }
    assert(device_protocol_line_feed(&assembler, '\n', &dropped) == DEVICE_PROTOCOL_LINE_READY);
    assert(strlen(assembler.line) == DEVICE_PROTOCOL_LINE_MAX - 1);

    /* One byte more: the whole line is dropped and reported once, with its size. */
    for (i = 0; i < DEVICE_PROTOCOL_LINE_MAX + 40; ++i) {
        assert(device_protocol_line_feed(&assembler, 'b', &dropped) == DEVICE_PROTOCOL_LINE_NONE);
    }
    assert(device_protocol_line_feed(&assembler, '\n', &dropped) == DEVICE_PROTOCOL_LINE_OVERFLOW);
    assert(dropped == DEVICE_PROTOCOL_LINE_MAX + 40);

    /* The tail of the oversized line is not parsed as a new line, and the
     * assembler recovers for the next one. */
    assert(feed_text(&assembler, "ok\n", &dropped) == DEVICE_PROTOCOL_LINE_READY);
    assert(strcmp(assembler.line, "ok") == 0);
}

static void test_claude_update_fixture(const char *dir)
{
    cJSON *frame = load_fixture(dir, "claude.update.json");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(frame, "payload");
    claude_snapshot_t snapshot;

    assert(strcmp(cJSON_GetObjectItemCaseSensitive(frame, "method")->valuestring,
                  DP_METHOD_CLAUDE_UPDATE) == 0);
    assert(device_protocol_parse_claude_update(payload, &snapshot));

    /* Every field the firmware reads must be present in what the host sends. */
    assert(snapshot.seq == 7);
    assert(snapshot.unread);
    assert(snapshot.updated_at_epoch_s == 1743957128);
    assert(strcmp(snapshot.session_id, "sess-1") == 0);
    assert(strcmp(snapshot.workspace, "project-foo") == 0);
    assert(strcmp(snapshot.title, "Awaiting approval") == 0);
    assert(strcmp(snapshot.detail, "Bash") == 0);
    assert(strcmp(snapshot.emotion, "happy") == 0);
    assert(strcmp(snapshot.event, "PermissionRequest") == 0);
    assert(strcmp(snapshot.permission_mode, "default") == 0);
    assert(snapshot.run_state == CLAUDE_RUN_WAITING_FOR_INPUT);
    assert(snapshot.pending_kind == CLAUDE_PENDING_APPROVAL);
    assert(strcmp(snapshot.pending_type, "Bash command") == 0);

    /* A null pending clears it; so does a kind this firmware does not know. */
    cJSON_ReplaceItemInObjectCaseSensitive(payload, "pending", cJSON_CreateNull());
    assert(device_protocol_parse_claude_update(payload, &snapshot));
    assert(snapshot.pending_kind == CLAUDE_PENDING_NONE);
    assert(snapshot.pending_type[0] == '\0');

    cJSON_ReplaceItemInObjectCaseSensitive(payload, "pending",
                                           cJSON_Parse("{\"kind\":\"future\",\"type\":\"x\"}"));
    assert(device_protocol_parse_claude_update(payload, &snapshot));
    assert(snapshot.pending_kind == CLAUDE_PENDING_NONE);
    assert(snapshot.pending_type[0] == '\0');

    cJSON_ReplaceItemInObjectCaseSensitive(
        payload, "pending", cJSON_Parse("{\"kind\":\"prompt\",\"type\":\"Question\"}"));
    assert(device_protocol_parse_claude_update(payload, &snapshot));
    assert(snapshot.pending_kind == CLAUDE_PENDING_PROMPT);
    assert(strcmp(snapshot.pending_type, "Question") == 0);

    /* Required fields are required. */
    cJSON_DeleteItemFromObjectCaseSensitive(payload, "seq");
    assert(!device_protocol_parse_claude_update(payload, &snapshot));
    cJSON_Delete(frame);
}

static void test_hello_fixture_matches_method_list(const char *dir)
{
    cJSON *frame = load_fixture(dir, "hello.json");
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(frame, "capabilities");
    size_t i = 0;

    assert(cJSON_IsArray(capabilities));
    assert((size_t)cJSON_GetArraySize(capabilities) == device_protocol_method_count);
    for (i = 0; i < device_protocol_method_count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(capabilities, (int)i);

        assert(cJSON_IsString(item));
        assert(strcmp(item->valuestring, device_protocol_methods[i]) == 0);
    }
    cJSON_Delete(frame);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_line_assembly();
    test_line_overflow_guard();
    test_claude_update_fixture(argv[1]);
    test_hello_fixture_matches_method_list(argv[1]);
    puts("device_protocol host tests: ok");
    return 0;
}
