package agentflow.dsl

import agentflow.jni.NativeBridge
import kotlinx.coroutines.flow.Flow

/**
 * Handle to a JSON-defined workflow loaded on the JVM. Each `run()` call
 * invokes the native bridge which loads + runs the workflow fresh; future
 * iterations will cache the parsed Workflow + share a Runner.
 */
class JsonWorkflow internal constructor(
    private val modelPath: String,
    private val json: String,
    private val tools: List<HostTool> = emptyList(),
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
     * The blocking native call is bridged to a cold Flow by [nativeTokenStream],
     * which owns the daemon worker thread, the hand-off queue and the
     * deadlock-safe cancel/release handling. Collect on whatever dispatcher you
     * like:
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
    fun streamTokens(userQuery: String): Flow<String> =
        nativeTokenStream { onToken, cancelId ->
            NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, onToken, cancelId)
        }

    /**
     * Constrained tool-mode run: the agent may call registered [tools], and
     * tool lifecycle events stream to [events] live. No token stream (the
     * constrained path has none); the full assistant reply is returned when
     * the run completes. The run blocks the calling thread — call from a
     * background dispatcher.
     *
     * Cancellation: call `nativeCancel(cancelId)` from another thread; the
     * per-run [CancellationSignal] handed to every tool `invoke` flips and the
     * run unwinds.
     */
    fun runConstrained(
        userQuery: String,
        events: RunEventCallback,
        cancelId: Long,
    ): String =
        NativeBridge.runJsonWorkflowConstrained(
            modelPath, json, tools.toTypedArray(), RunSignal(),
            userQuery, events, cancelId)
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

/**
 * Loads a workflow with host-side tools registered. Names in the workflow
 * JSON's `"tools"` arrays must match the [tools]' [HostTool.name]s.
 */
fun loadWorkflow(
    modelPath: String,
    json: String,
    tools: List<HostTool>,
): JsonWorkflow = JsonWorkflow(modelPath, json, tools)
