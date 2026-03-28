#include "chat-auto-parser-helpers.h"
#include "chat-auto-parser.h"
#include "chat-peg-parser.h"
#include "chat.h"
#include "peg-parser.h"
#include "testing.h"

#include <iostream>
#include <string>
#include <vector>

using namespace autoparser;

static void test_pythonic_grammar_generation(testing & t) {
    // 1. Setup tools
    nlohmann::ordered_json tools = nlohmann::ordered_json::array();
    tools.push_back({
        {"type", "function"},
        {"function", {
            {"name", "get_weather"},
            {"description", "Get the weather"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"location", {{"type", "string"}}},
                    {"unit", {{"type", "string"}, {"enum", {"celsius", "fahrenheit"}}}}
                }},
                {"required", {"location", "unit"}}
            }}
        }}
    });

    // 2. Setup a dummy Llama 3.2-like template
    std::string template_src = 
        "{%- if tools is not none %}"
        "Environment: ipython\n"
        "You have access to the following functions: \n"
        "{%- for t in tools %}{{ t | tojson }}\n\n{%- endfor %}"
        "{%- endif %}"
        "<|start_header_id|>user<|end_header_id|>\n\n{{ messages[0].content }}<|eot_id|>"
        "<|start_header_id|>assistant<|end_header_id|>\n\n";

    common_chat_template tmpl(template_src, "128000", "128001");

    generation_params inputs;
    inputs.tools = tools;
    inputs.messages.push_back({ "user", "What is the weather in SF?" });
    inputs.chat_template_tool_format = "pythonic";
    inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    // 3. Generate parser
    peg_generator generator;
    common_chat_params result = generator.generate_parser(tmpl, inputs);

    // 4. Verify grammar
    t.test("grammar contains pythonic markers", [&](testing & t) {
        // Rule names use hyphens
        t.assert_true("grammar has tool rule", result.grammar.find("tool-get-weather") != std::string::npos);
        // The pythonic generator uses "[" as start
        t.assert_true("grammar has [ start", result.grammar.find("\"[\"") != std::string::npos);
        t.assert_true("grammar has ] end", result.grammar.find("\"]\"") != std::string::npos);
        t.assert_true("grammar has function name", result.grammar.find("\"get_weather\"") != std::string::npos);
        t.assert_true("grammar has arg name location", result.grammar.find("\"location\"") != std::string::npos);
        t.assert_true("grammar has arg name unit", result.grammar.find("\"unit\"") != std::string::npos);
    });

    // 5. Verify parsing
    t.test("parsing pythonic tool call", [&](testing & t) {
        common_peg_arena arena;
        arena.load(result.parser);
        
        // Input should ONLY be the generated part if the grammar root is tool-call
        // Testing with double quotes since model used them
        std::string input = "[get_weather(location=\"San Francisco, CA\", unit=\"celsius\")]";
        common_peg_parse_context ctx(input);
        auto parse_result = arena.parse(ctx);
        
        t.assert_true("parsing successful", parse_result.success());
        if (!parse_result.success()) return;
        
        common_chat_msg msg;
        common_chat_peg_mapper mapper(msg);
        mapper.from_ast(ctx.ast, parse_result);

        t.assert_true("one tool call found", msg.tool_calls.size() == 1);
        if (msg.tool_calls.empty()) return;
        
        t.assert_equal("correct tool name", std::string("get_weather"), msg.tool_calls[0].name);
        
        json args = json::parse(msg.tool_calls[0].arguments);
        t.assert_equal("correct location", std::string("San Francisco, CA"), args["location"].get<std::string>());
        t.assert_equal("correct unit", std::string("celsius"), args["unit"].get<std::string>());
    });
}

int main() {
    testing t(std::cout);
    t.verbose = true;

    t.test("pythonic tool calls", test_pythonic_grammar_generation);

    return t.summary();
}
