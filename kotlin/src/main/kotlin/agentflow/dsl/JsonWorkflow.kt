package agentflow.dsl

import agentflow.jni.NativeBridge

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
