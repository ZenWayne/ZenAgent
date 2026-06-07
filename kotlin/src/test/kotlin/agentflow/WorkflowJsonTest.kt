package agentflow

import agentflow.dsl.loadWorkflow
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
}
