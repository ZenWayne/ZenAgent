package agentflow

import agentflow.dsl.CancellationSignal
import agentflow.dsl.HostTool
import agentflow.dsl.RunEventCallback
import agentflow.dsl.loadWorkflow
import agentflow.jni.NativeBridge
import java.util.concurrent.CopyOnWriteArrayList
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Assumptions.assumeTrue
import org.junit.jupiter.api.Test

/**
 * JVM smoke test for the host-tool bridge: registers a Kotlin [HostTool],
 * runs a constrained workflow against a real LiteRT-LM model, and asserts
 * the upcall round-trips (tool called with its call id, result returned,
 * lifecycle events delivered). Skipped when MODEL_PATH is unset.
 */
class HostToolBridgeTest {
    private val json = """
        {
          "schema_version":1,"name":"bridge_test","version":"v1",
          "state":{"kind":"dynamic_json","fields":{}},
          "agents":{
            "main":{
              "system_prompt":
                "Use the echo tool when asked to echo something. Reply with the tool's result only.",
              "model":{"max_output_tokens":64,"constrained_tool_calls":true},
              "tools":["echo"]
            }
          },
          "main":"main"
        }
    """.trimIndent()

    private class EchoTool : HostTool {
        override val name = "echo"
        override val description = "Echoes its input text."
        override val paramsJsonSchema =
            """{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}"""
        override val requiresApproval = false
        override fun invoke(
            toolCallId: String,
            argsJson: String,
            cancel: CancellationSignal,
        ): String = """{"echoed":$argsJson,"callId":"$toolCallId"}"""
    }

    @Test
    fun hostToolUpcallRoundTrips() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val toolCalls = CopyOnWriteArrayList<String>()
        val returns = CopyOnWriteArrayList<String>()
        val wf = loadWorkflow(modelPath!!, json, listOf(EchoTool()))
        val cancelId = NativeBridge.nativeNewCancel()
        val reply = wf.runConstrained(
            "Echo hello",
            object : RunEventCallback {
                override fun onToolCall(toolCallId: String, name: String, argsJson: String) {
                    toolCalls.add("$name:$toolCallId:$argsJson")
                }
                override fun onToolReturn(toolCallId: String, resultJson: String) {
                    returns.add("$toolCallId:$resultJson")
                }
            },
            cancelId,
        )
        NativeBridge.nativeFreeCancel(cancelId)
        assertTrue(reply.isNotBlank(), "expected non-empty reply, got: $reply")
        assertEquals(1, toolCalls.size, "expected one tool call, got $toolCalls")
        assertEquals(1, returns.size, "expected one tool return, got $returns")
        assertTrue(returns[0].contains("callId"), "upcall should carry toolCallId")
        assertTrue(returns[0].contains("hello"), "upcall should carry args: ${returns[0]}")
    }
}
