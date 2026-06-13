package agentflow

import agentflow.dsl.loadWorkflow
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Assumptions.assumeTrue
import org.junit.jupiter.api.Test

/**
 * JVM smoke test: loads a JSON-defined workflow over JNI and runs the main
 * agent against a real LiteRT-LM model. Skipped when MODEL_PATH is unset so
 * CI stays green without a model checkpoint.
 */
class WorkflowJsonTest {
    @Test
    fun jsonWorkflowReturnsReply() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val json = """
        {
          "schema_version":1,"name":"jvm_test","version":"v1",
          "state":{"kind":"dynamic_json","fields":{}},
          "agents":{
            "main":{"system_prompt":"Reply in one short sentence.",
                    "model":{"max_output_tokens":64},
                    "tools":[]}
          },
          "main":"main"
        }
        """.trimIndent()

        val wf = loadWorkflow(modelPath!!, json)
        val reply = wf.run("Say hello.")
        println("Reply: $reply")
        assertFalse(reply.isBlank(), "expected non-empty reply")
    }

    @Test
    fun jsonWorkflowStreamsTokens() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val json = """
        {
          "schema_version":1,"name":"jvm_stream_test","version":"v1",
          "state":{"kind":"dynamic_json","fields":{}},
          "agents":{
            "main":{"system_prompt":"Reply in one short sentence.",
                    "model":{"max_output_tokens":64},
                    "tools":[]}
          },
          "main":"main"
        }
        """.trimIndent()

        val wf = loadWorkflow(modelPath!!, json)
        val deltas = mutableListOf<String>()
        val reply = wf.runStreaming("Say hello.") { delta -> deltas.add(delta) }

        println("Streamed ${deltas.size} deltas; reply: $reply")
        assertFalse(reply.isBlank(), "expected non-empty reply")
        assertTrue(deltas.size > 1, "expected real per-token streaming (>1 delta)")
        // The concatenated deltas should reconstruct the streamed reply text.
        assertTrue(
            reply.contains(deltas.joinToString("").trim().take(8)),
            "reply should contain the streamed token text",
        )
    }

    @Test
    fun jsonWorkflowStreamsTokensAsFlow() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val json = """
        {
          "schema_version":1,"name":"jvm_flow_test","version":"v1",
          "state":{"kind":"dynamic_json","fields":{}},
          "agents":{
            "main":{"system_prompt":"Reply in one short sentence.",
                    "model":{"max_output_tokens":64},
                    "tools":[]}
          },
          "main":"main"
        }
        """.trimIndent()

        val wf = loadWorkflow(modelPath!!, json)
        val deltas = runBlocking { wf.streamTokens("Say hello.").toList() }
        val text = deltas.joinToString("")
        println("Flow streamed ${deltas.size} deltas: $text")
        assertTrue(deltas.size > 1, "expected real per-token streaming via Flow")
        assertFalse(text.isBlank(), "expected non-empty streamed text")
    }

    @Test
    fun jsonWorkflowStreamingCancels() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val json = """
        {
          "schema_version":1,"name":"jvm_cancel_test","version":"v1",
          "state":{"kind":"dynamic_json","fields":{}},
          "agents":{
            "main":{"system_prompt":"Reply in one long paragraph.",
                    "model":{"max_output_tokens":256},
                    "tools":[]}
          },
          "main":"main"
        }
        """.trimIndent()

        val wf = loadWorkflow(modelPath!!, json)
        // take(1) cancels the flow after the first delta, which signals the
        // native run to stop. The test passing (not hanging) exercises the
        // cooperative-cancellation path end to end.
        val first = runBlocking { wf.streamTokens("Tell me a long story.").take(1).toList() }
        println("Cancelled after ${first.size} delta(s): ${first.joinToString("")}")
        assertEquals(1, first.size, "take(1) should yield exactly one delta")
        assertFalse(first[0].isBlank(), "first delta should be non-empty")
    }
}
