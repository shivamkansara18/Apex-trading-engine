# Portfolio positioning and interview preparation

Apex is strongest as a systems/performance engineering project. It demonstrates
C++ ownership, data structures, explicit contracts, reproducible experiments and
failure reasoning. Research-oriented applications can emphasize simulation and
experimental discipline; the project does not establish a profitable signal.

## Fit to the roles you named

Official material reviewed November 16, 2026:

- Jane Street's software-engineering preparation page emphasizes collaborative
  coding and explaining a solution in a real language. Its NYC role highlights
  programming ability and technical curiosity, and says prior OCaml/finance
  experience is not required. This supports emphasizing reasoning and code quality,
  rather than adding a language or finance keyword merely for a job description.
  [Interview preparation](https://www.janestreet.com/preparing-for-a-software-engineering-interview/),
  [NYC software engineering](https://www.janestreet.com/join-jane-street/position/4274288002/).
- Citadel's engineering FAQ describes coding, system design, problem solving and
  explaining trade-offs. The ownership, recovery and measurement decisions here
  provide concrete discussion material, but don't replace algorithmic preparation.
  [Engineering FAQ](https://www.citadel.com/careers/career-perspectives/faqs-engineering-at-citadel/).
- Jane Street describes quantitative research in terms of data analysis, building
  and testing models and developing strategies. Reproducible systems experiments
  show part of that discipline, but this simulator has no validated forecasting
  or strategy-return result. Present that gap accurately.
  [Quantitative research](https://www.janestreet.com/quantitative-research/).

These are role-fit inferences, not employer endorsements or a hiring prediction.
Roles and criteria can change. No application or recruiter message has been sent.

## A 60-second engineering explanation

“I built a C++20 exchange simulator to investigate matching correctness and memory
costs. One writer owns each book: ordered price levels, FIFO lists and an ID index
support price-time matching and cancellation. I kept integer quantities/prices,
explicit rejection semantics and deterministic journals. Profiling showed risk
checks constructing owning snapshots; a direct read-only traversal improved paired
core throughput by about 1.95–2.19× on three saved inputs without changing outcomes.
I added a structurally independent vector/sort model to test shared matching logic,
plus network overload, recovery and partition-isolation tests. The measurements
are synthetic and core-only; I don't claim exchange/network latency.”

Adapt the wording to your own understanding and decisions. The detailed scope
lives in the code and evidence, not in this script.

## A 60-second research explanation

“I used a deterministic order-book simulator as a controlled computational
experiment. I held saved inputs and build/host constant, interleaved five paired
blocks per implementation policy and audited 7.5 million raw timing samples.
Event/final-state signatures checked that candidates were doing equivalent work.
I reported paired ratios and tail behavior, preserved the measured source and
kept negative results. It's systems/microstructure simulation work, not evidence
of alpha or calibrated real-market behavior.”

## Questions to prepare, with the reasoning target

| Question | What your answer should establish |
| --- | --- |
| Why tree + FIFO lists + ID index? | Price selection, stable nodes, indexed erase and iterator lifetime. Average hash lookup is not worst-case O(1). |
| Where is matching complexity paid? | Price selection, fills/removals, allocation and risk traversal; cancel storage complexity alone isn't end-to-end command cost. |
| Why one writer? | Unambiguous mutation order and fewer shared-state races; cross-book parallelism doesn't require competing writers per book. |
| Why integer ticks? | Explicit scale and overflow checks; floating prices are not needed to represent supported prices. |
| Why validate market risk before fills? | Rejections preserve the entire book, including multi-level sweeps and self-trade boundaries. |
| Why does replace reset priority? | Explicit business policy; reject first, then cancel old and admit the desired remaining quantity with new priority. |
| What did profiling establish? | A sampled allocation-heavy path, followed by controlled comparisons—not an exact allocation count from the profile. |
| What did the optimization change? | Owning snapshots removed from risk validation; no second usage ledger or business-rule change. |
| Why not promote the fastest flat policy? | Shallow measured books do not establish O(P) level-update behavior on deep books. |
| Why reject small event buffers? | Large-fill tails regressed; median throughput alone was insufficient. |
| What can the independent oracle miss? | Common specification errors, intermediate event bugs and unmodeled risk/admin/overflow behavior. |
| Why can a network disconnect be ambiguous? | Matching may commit before output delivery; open snapshots cannot prove every terminal command outcome. |
| What does sealed replay prove? | Recorded inputs regenerate matching outputs/state; it isn't live crash recovery or a proof of business correctness. |
| Why no automatic retry? | No durable idempotency key/outcome ledger; blind retry can duplicate intent. |
| What makes the agents adaptive? | Current depth/inventory and seeded action choices; no learned strategy or financial calibration. |
| Can eight processes prove eightfold scaling? | No. Independent books permit concurrency; actual capacity/latency requires load measurement. |

## Practice without the repository open

1. Implement a minimal FIFO limit matcher and trace two partial fills at resting prices.
2. Extend it with cancel/replace and write a rejection-atomicity example.
3. Explain iterator invalidation in the production tree/list/index design.
4. Derive quantity conservation for partial fills, cancel and replacement.
5. Write a bounded MPSC queue with a mutex and articulate the ownership handoff.
6. Decode a split TCP frame and preserve an unsent write suffix.
7. Calculate paired speed ratios and explain why their median can differ from the
   ratio of two medians. Explain coordinated-omission limits of a closed-loop harness.
8. Describe the minimum protocol/data model needed for durable idempotent retries.

Your ability to reason through these is more important than memorizing the exact
README. Follow each employer's rules for tools during interviews. Describe your
actual development process and personal contributions accurately if asked.

## Application readiness

- Use one of the separate [resume project sections](resume-project.md), keeping
  the role-specific emphasis and core-only measurement qualification.
- Make the [five-minute reviewer path](reviewer-guide.md) easy to find.
- Retain source/raw evidence and verify the current release before sharing it.
- Your earlier preference is local-only. A portable ZIP is prepared; a public
  repository, remote CI and public demo are not created without changing that scope.
  When you choose to share, add a real reviewed link to the resume—never a placeholder.
- Education, work history, eligibility, communication and interview preparation
  still affect outcomes. A project cannot ensure a resume screen or interview.
