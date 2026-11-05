#include <cstdio>
#include <atomic>

#include "llm/openai.h"
#include "live_llm.h"

int main() {
    if (!liveLlmEnabled()) {
        printf("llm_test: skipped (set MINIAGENT_LIVE_LLM=1 to send real requests)\n");
        return 0;
    }

    OpenAIProvider provider;
    std::atomic<bool> abort{false};

    // Read-only /models probe: prints the detected context window (best
    // effort — 0 when the server exposes no such field).
    const int win = probeContextWindow(provider.baseUrl, provider.apiKey, provider.model);
    printf("probeContextWindow: %d\n", win);

    std::vector<AgentMessage> history{UserMessage{"Say hello in one short sentence."}};

    printf("Streaming: ");
    AssistantMessage msg = provider.stream(
        "You are a helpful assistant.",
        history,
        {},
        [](const std::string& d) {
            printf("%s", d.c_str());
            fflush(stdout);
        },
        [](const std::string& d) {
            printf("\x1b[3m[thinking] %s\x1b[0m", d.c_str());
            fflush(stdout);
        },
        abort
    );
    printf("\n\n---\nstopReason: %s\nerror: %s\ntext: %s\nthinking: %s\nin/out tokens: %d/%d\n",
           msg.stopReason.c_str(),
           msg.errorMessage.c_str(),
           msg.text.c_str(),
           msg.thinking.c_str(),
           msg.usage.inputTokens,
           msg.usage.outputTokens);

    if (msg.stopReason != "stop" || msg.text.empty()) {
        printf("llm_test: FAILED\n");
        return 1;
    }
    printf("llm_test: passed\n");
    return 0;
}
