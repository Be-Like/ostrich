---
name: write-a-ard
description: Create a ARD through user interview, codebase exploration and then write the ARD to the project. Use when user wants to write a ARD, create a architecture review document, or architecturally plan a new feature.
---

## Process

1. The projects context should be in the conversation. If it isn't, read context/projects/README.md

2. If the PRD is not in the conversation. Check if there is a PRD in the project and read it if there is. If there is not, just ask the user for what this project is focused on.

3. If you have not already explored the codebase, do so to understand the current architecture, existing patterns, and integration layers.

4. Interview the user relentlessly about every aspect of this plan until you reach a shared understanding. Walk down each branch of the design tree, resolving dependencies between decisions one-by-one. ASK ONE QUESTION AT A TIME.

5. Sketch out the major modules you will need to build or modify to complete the implementation. Actively look for opportunities to extract deep modules that can be tested in isolation.

A deep module (as opposed to a shallow module) is one which encapsulates a lot of functionality in a simple, testable interface which rarely changes.

Check with the user that these modules match their expectations. Check with the user which modules they want tests written for.

6. Once you have a complete understanding of the problem, solution, architecture, proposed changes, and how they fit in or would modify the existing architecture, use the template below to write the ARD to the project. Make sure that we hard wrap the lines at a maximum width of 72 characters so that it is easy to read in a terminal.

<ard-template>

## PRD

A reference to the PRD that was evaluated to create this. If there was no PRD simply put the summary of the goal of this effort here.

## Explanation Architectural Components

This should be an explanation of Modules in play with the change, how they currently relate to each other, and any changes that would have to occure to facilitate the solution.

## Interfaces

In this section it outlines the different interfaces involved. This could be a new payload structure for a new endpoint in an http based API or it could be the interface of a module.

## Out of Scope

A description of the things that are out of scope for this ARD.

## Further Notes

Any further notes about the feature.

</ard-template>
