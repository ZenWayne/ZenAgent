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
        // cancelId 0 = no cancellation handle (blocking call, no external handle).
        NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, { delta ->
            onToken(delta)
        }, 0L)

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
     * Cooperative cancellation: cancelling the collector (or e.g. `take(n)`)
     * signals the native run via a cancel handle, which breaks the in-flight
     * engine request so the run stops promptly instead of finishing in the
     * background.
     */
    fun streamTokens(userQuery: String): Flow<String> = callbackFlow {
        val cancelId = NativeBridge.nativeNewCancel()
        val job = launch(Dispatchers.IO) {
            try {
                NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, {
                    delta ->
                    trySend(delta)
                }, cancelId)
                close()
            } catch (t: Throwable) {
                close(t)
            } finally {
                // Token already taken by the run; safe to release the source.
                NativeBridge.nativeFreeCancel(cancelId)
            }
        }
        awaitClose {
            // Signal the (possibly still-running) native call to stop. No-op if
            // already finished/freed — registry guards against a dangling id.
            NativeBridge.nativeCancel(cancelId)
            job.cancel()
        }
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
