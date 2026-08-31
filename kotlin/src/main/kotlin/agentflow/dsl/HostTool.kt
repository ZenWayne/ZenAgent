package agentflow.dsl

/**
 * Cheap per-run cancellation probe, visible to tool implementations.
 *
 * The native side flips `cancelled` when the run's cancel token fires (via a
 * JNI worker-thread SetBooleanField on the [RunSignal] instance); a tool's
 * blocking `invoke` polls it to bail out early.
 */
interface CancellationSignal {
    val cancelled: Boolean
}

/**
 * A tool implemented on the app side, executed by the native agent loop
 * (spec §4.1). Register instances via `loadWorkflow(modelPath, json, tools)`;
 * the workflow JSON's `"tools"` array must name them.
 *
 * Implementations must be **concurrency-safe**: the parallel dispatch loop
 * may invoke the same tool concurrently from different worker threads.
 */
interface HostTool {
    val name: String
    val description: String

    /** JSON Schema for arguments — drives constrained tool-call decoding. */
    val paramsJsonSchema: String

    /** Side-effecting tools (writes, code execution) → true (approval gate). */
    val requiresApproval: Boolean

    /**
     * Blocking invocation. Runs on a dedicated JVM worker thread owned by the
     * JNI bridge — never on the runner's single thread. Implementations may
     * bridge to suspend internally. [toolCallId] is the model-assigned call
     * id (used to correlate an approval gate with its tool-call card).
     * Return a JSON result string; `{"error":"..."}` on failure.
     */
    fun invoke(toolCallId: String, argsJson: String, cancel: CancellationSignal): String
}

/**
 * Live run events (spec §4.1). Tool mode is constrained decoding — no token
 * stream — so only the tool lifecycle methods fire there; `onToken` is for
 * the unconstrained streaming path. All three have default no-op bodies so
 * callers implement only what they need.
 */
interface RunEventCallback {
    fun onToken(token: String) {}
    fun onToolCall(toolCallId: String, name: String, argsJson: String) {}
    fun onToolReturn(toolCallId: String, resultJson: String) {}
}

/**
 * DSL-internal signal; the JNI flips `cancelled` when nativeCancel fires.
 * One instance per `runConstrained` call, passed to every tool upcall.
 */
internal class RunSignal : CancellationSignal {
    @Volatile
    override var cancelled: Boolean = false
}
