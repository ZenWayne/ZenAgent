package agentflow.dsl

import agentflow.jni.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch

/**
 * Handle to a JSON-defined workflow loaded on the JVM. Each `run()` call
 * invokes the native bridge which loads + runs the workflow fresh; future
 * iterations will cache the parsed Workflow + share a Runner.
 */
class JsonWorkflow internal constructor(
    private val modelPath: String,
    private val json: String,
) {
    fun run(userQuery: String): String =
        NativeBridge.runJsonWorkflow(modelPath, json, userQuery)

    /**
     * Streaming run: [onToken] is invoked with each generated text delta as it
     * arrives; the full assistant reply is returned when the run completes.
     *
     * ```
     * wf.runStreaming("hello") { delta -> print(delta) }
     * ```
     */
    fun runStreaming(userQuery: String, onToken: (String) -> Unit): String =
        NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery) { delta ->
            onToken(delta)
        }

    /**
     * Streaming run exposed as a cold [Flow] of text deltas.
     *
     * The native run is blocking (it drives its own event loop to completion),
     * so it runs on [Dispatchers.IO]; each delta is forwarded into the flow via
     * `trySend`. The flow completes when the run finishes and fails if the
     * native call throws. Collect on whatever dispatcher you like:
     *
     * ```
     * wf.streamTokens("hello").collect { delta -> print(delta) }
     * ```
     *
     * Note: the underlying native run has no cancellation hook yet, so
     * cancelling the collector stops delivery but the run continues in the
     * background until it completes.
     */
    fun streamTokens(userQuery: String): Flow<String> = callbackFlow {
        val job = launch(Dispatchers.IO) {
            try {
                NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery) {
                    delta ->
                    trySend(delta)
                }
                close()
            } catch (t: Throwable) {
                close(t)
            }
        }
        awaitClose { job.cancel() }
    }
}

/**
 * Loads a workflow from raw JSON.
 *
 * Example:
 * ```
 * val wf = loadWorkflow("/path/to/model.litertlm", workflowJsonString)
 * println(wf.run("hello"))
 * ```
 */
fun loadWorkflow(modelPath: String, json: String): JsonWorkflow =
    JsonWorkflow(modelPath, json)
