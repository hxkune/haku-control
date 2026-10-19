# Haku Control on Discord

Server: [Haku | Setups](https://discord.gg/VduV69nm6).

Setup status: category, all six channels, the Haku Control role and the Haku Control
News webhook have been created. English topics and forum guidelines are saved;
the welcome message is published. Release news uses PNG cards posted through
GitHub Actions. Carl-bot reaction roles are configured; adding
the check-mark reaction was verified to grant the Haku Control role, and removing
it was verified to revoke the role.

## Channels

The **HAKU CONTROL** category denies View Channels to @everyone and allows it
for **Haku Control**. General, help, bug-reports and feature-requests inherit
these permissions. News has matching visibility overrides and remains read-only.
Start-here explicitly allows @everyone to view, read history and add reactions.

| Channel | Type | Topic |
| --- | --- | --- |
| start-here | Text, staff posts only | Meet Haku Control: downloads, setup, supported devices and useful links. |
| news | Announcement, staff and webhook posts only | Haku Control releases, improvements and project announcements. |
| general | Text | Share your lighting setups, effects and everyday Haku Control experiences. |
| help | Forum | Need a hand? Start a post with your app version, device model and what you have tried. |
| bug-reports | Forum | Found a bug? Include steps to reproduce, expected behaviour and what actually happened. |
| feature-requests | Forum | Suggest a feature or device integration. Describe what you want to do and why. |

If forums are unavailable, use text channels with one thread per question or report.
For the two staff-only channels, prevent members from sending messages or creating
threads; check role overrides as well as the @everyone permissions.

## Role

Name: **Haku Control**. Colour: purple (`#A78BFA`).

A community interest role for Haku Control users. No additional administrative or
moderation permissions. Keep public role mentions disabled; staff can mention the
role for important announcements. Do not assign it to everyone automatically.
Carl-bot grants this role for the ✅ reaction on the
[welcome message](https://discord.com/channels/1260308779258351636/1561019190310207548/1561020150822609008).
Removing the reaction is configured to remove the role. Keep Carl-bot above
Haku Control in the role hierarchy.

## Welcome message for start-here

**Unlock the Haku Control channels**

React with ✅ below to get the **Haku Control** role and unlock news, chat, help,
bug reports and feature requests. Remove your reaction to leave the role.

**Welcome to Haku Control**

One lighting scene for your PC and your room. Haku Control brings your supported
PC RGB hardware and room lights together with shared effects, colours and profiles.

**Get started**
1. [Download Haku Control for Windows](https://hakune.blog/haku-control.html).
2. Check [supported devices](https://hakune.blog/haku-control.html#devices) and follow the [installation guide](https://hakune.blog/haku-control.html#install).
3. Open the app, scan for devices and create your first lighting profile.

**Find your channel**
- **news** — releases and project announcements.
- **general** — setups, effects and community chat.
- **help** — installation, pairing and everyday questions.
- **bug-reports** — problems we can reproduce and investigate.
- **feature-requests** — ideas and requests for device support.

Haku Control is under active development. Device support varies; check the support
table before installing. Some integrations have only been tested with simulators.

Please write in English, be respectful and keep each support topic in its own
post or thread. Remove passwords, tokens and other private information from logs
and screenshots before sharing them.

## Forum prompts

### help

**What do you need help with?**
**Haku Control version:**
**Windows version:**
**Device brand and exact model:**
**What have you tried?**
**Screenshot or relevant log excerpt (remove private information):**

### bug-reports

**Summary:**
**Haku Control version:**
**Windows version and device model:**
**Steps to reproduce:**
1.
2.
3.
**Expected behaviour:**
**Actual behaviour:**
**Screenshots or relevant logs (remove private information):**

### feature-requests

**What would you like to do?**
**Why would it help?**
**Device brand and exact model (if relevant):**
**Current workaround:**

## Automatic release news

The **Discord news cards** workflow renders a 1200 × 800 PNG for each newly
published stable release and uploads it using the **Haku Control News** incoming
webhook. Store its base URL (without `/github`) in the repository Actions secret
`DISCORD_NEWS_WEBHOOK_URL`. Disable the previous direct GitHub webhook
`700008557` so releases do not produce duplicate plain notifications.

The card contains the release version, date and up to three changes from its
description. Write those changes in English; there is no automatic translation.
If release notes contain only a changelog link, the card simply announces the
new Windows build. Both the image footer and the clickable download link point
to the website. A text description accompanies the image for accessibility.

Pull requests run the renderer tests and upload a preview without publishing.
To preview the latest release manually, run the workflow with **publish** off;
download `haku-control-news-card` from the run's artifacts. To announce the latest
release manually, turn **publish** on. A manual run or a rerun can post a duplicate:
check **news** before retrying a failed or timed-out publication.

The public repository's releases are published by the private Pro release
workflow. If release creation moves to this repository's `GITHUB_TOKEN`, call
the card workflow explicitly: GitHub suppresses release-triggered workflows
for events produced by that token.

Keep the webhook URL private: it grants permission to post to the channel. Do not
put it in this repository, screenshots or public messages.

References: [Discord file uploads](https://docs.discord.com/developers/resources/webhook#execute-webhook),
[GitHub workflow triggers](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).
