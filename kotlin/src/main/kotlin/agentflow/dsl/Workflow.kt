package agentflow.dsl

import agentflow.jni.NativeBridge

/**
 * Per-agent configuration. MVP supports one agent per workflow; multi-node
 * graphs land in P10.
 */
class AgentBuilder internal constructor(val id: String) {
    var modelPath: String = ""
    var systemPrompt: String = "You are a helpful assistant."
    var constrainedToolCalls: Boolean = false
}

/**
 * Workflow scope. The MVP DSL captures exactly one [AgentBuilder] block;
 * `edges {}` and multi-agent topologies are reserved for later phases.
 */
class WorkflowBuilder internal constructor() {
    internal var agent: AgentBuilder? = null

    fun agent(id: String, block: AgentBuilder.() -> Unit): AgentBuilder {
        val b = AgentBuilder(id).apply(block)
        agent = b
        return b
    }
}

/**
 * Compiled workflow. Holds the agent config and dispatches to the C++
 * runtime on [run].
 */
class Workflow internal constructor(private val agent: AgentBuilder) {
    /**
     * Synchronously runs the agent against [userQuery]. Blocks until the
     * model finishes and returns the assistant reply.
     *
     * The MVP path is sync-only; streaming/Flow lands in P10.
     */
    fun run(userQuery: String): String {
        require(agent.modelPath.isNotEmpty()) {
            "agent.modelPath must be set"
        }
        return NativeBridge.runAgent(
            modelPath = agent.modelPath,
            systemPrompt = agent.systemPrompt,
            userQuery = userQuery,
            constrainedToolCalls = agent.constrainedToolCalls,
        )
    }
}

/**
 * Entry-point. Usage:
 * ```
 * val wf = workflow {
 *     agent("chat") {
 *         modelPath = "/path/to/model.litertlm"
 *         systemPrompt = "Reply briefly."
 *     }
 * }
 * val reply = wf.run("Say hello.")
 * ```
 */
fun workflow(block: WorkflowBuilder.() -> Unit): Workflow {
    val b = WorkflowBuilder().apply(block)
    val agent = requireNotNull(b.agent) {
        "workflow must declare at least one agent {} block"
    }
    return Workflow(agent)
}
