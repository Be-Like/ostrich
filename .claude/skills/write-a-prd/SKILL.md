---
name: write-a-prd
description: Create a PRD through user interview, codebase exploration and then write the PRD to the project. Use when user wants to write a PRD, create a product requirements document, or plan a new feature.
---

This skill will be invoked when the user wants to create a PRD. You may skip steps if you don't consider them necessary.

1. The projects context should be in the conversation. If it isn't, read context/projects/README.md

2. Ask the user for a long, detailed description of the problem they want to solve and any potential ideas for solutions.

3. Explore the repo to verify their assertions and understand the current state of the codebase.

4. Interview the user relentlessly about their concept in relation to the existing application until you reach a shared understanding.

5. Once you have a complete understanding of the problem and solution, use the template below to write the PRD.
The PRD should be written to the project. It must hard wrap the lines to a maximum width of 72 characters so that it is easily read in a terminal.

<prd-template>

## Problem Statement

The problem that the user is facing, from the user's perspective.

## Solution

The solution to the problem, from the user's perspective.

## User Stories

A LONG, numbered list of user stories. Each user story should be in the format of:

1. As an <actor>, I want a <feature>, so that <benefit>

<user-story-example>
1. As a mobile bank customer, I want to see balance on my accounts, so that I can make better informed decisions about my spending
</user-story-example>

This list of user stories should be extremely extensive and cover all aspects of the feature.

## Out of Scope

A description of the things that are out of scope for this PRD.

## Further Notes

Any further notes about the feature.

</prd-template>
