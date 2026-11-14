---
description: Enforce zero-assumed-knowledge first-principles pedagogy, interactive step-through animations, and clean light-themed visual-first design in all architecture and educational documentation.
globs: ["**/*.html", "docs/**"]
---

# Visual-First First-Principles Pedagogy Invariant

When authoring architectural guides, conceptual explainers, or educational technical documentation:

1. **Strict Bottom-Up Pedagogy (Zero Assumed Knowledge)**:
   - Never use technical jargon (e.g., *garbage collector*, *heap*, *pointer*, *cache line*, *write amplification*, *tombstone*, *SIMD*, *cosine similarity*) without introducing its absolute physical foundation first.
   - Start from atomic physical reality: what is a bit (voltage high/low), a byte (8 bits), RAM (numbered mailboxes), CPU (the calculator reading mailboxes), persistent storage (flash gates / magnetic platters), and program execution.
   - The reader must NEVER need to open a search engine to understand any single term. Every term must be defined in-place with intuitive physical analogies before it is used in architectural diagrams.

2. **Visuals & Animations First (Eliminate Text Walls)**:
   - Replace dense paragraphs with visual step-by-step animated diagrams, memory address grids, bit-level packet inspectors, and interactive state transitions.
   - Provide interactive "Step Through" controls (sliders, next/prev step buttons, interactive packet simulators) allowing users to see data flow through memory, disk, and CPU visually.

3. **Clean Light / White Editorial Design**:
   - Default to a clean, modern, high-contrast white/light theme (like Stripe, Brilliant.org, or Apple technical documentation): crisp dark typography, clear subtle borders, ample white space, and distinct accent colors. Avoid dark, cluttered, or "vibe coded" unreadable templates.

4. **Concrete Codebase Mapping**:
   - Connect every visual model directly to the exact file paths and C++ structs in the repository so the learner sees how the physical concept manifests in actual production code.
