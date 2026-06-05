package agentflow

import agentflow.dsl.workflow
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assumptions.assumeTrue
import org.junit.jupiter.api.Test

/**
 * JVM smoke test: drives a real LiteRT-LM model through the JNI bridge
 * via the Kotlin DSL. Skipped when MODEL_PATH is unset so the test suite
 * stays green in environments without a model file.
 */
class SmokeTest {
    @Test
    fun realModelAnswersHello() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null, "MODEL_PATH not set — skipping")

        val flow = workflow {
            agent("chat") {
                this.modelPath = modelPath!!
                systemPrompt = "Reply in one short sentence."
            }
        }
        val reply = flow.run("Say hello.")
        println("Reply: $reply")
        assertFalse(reply.isBlank(), "expected non-empty reply")
    }
}
