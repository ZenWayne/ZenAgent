package agentflow.jni

/**
 * Thin JNI surface. Loads libagentflow_jni.so on first reference and
 * forwards external calls into the C++ runtime.
 *
 * Object is internal — Kotlin callers should go through the DSL in
 * [agentflow.dsl] instead of calling these directly.
 */
internal object NativeBridge {
    init {
        System.loadLibrary("agentflow_jni")
    }

    /**
     * Synchronous one-agent run. Mirrors examples/agent-demo: build a
     * single-node graph, run the AgentNode's ReAct loop, return the
     * assistant_reply field as a UTF-8 string.
     *
     * Throws RuntimeException (from the C++ side) on engine creation or
     * inference errors.
     */
    external fun runAgent(
        modelPath: String,
        systemPrompt: String,
        userQuery: String,
        constrainedToolCalls: Boolean,
    ): String
}
