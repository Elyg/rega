# Sequence: a render coming in. Frames appear as they're written, gaps held or
# skipped (G), the last frame still half written.
Setup cp wip.100[1-5].exr "$WORK/"
Run rvtui "$WORK/wip.####.exr"
Spawn for f in wip.10{09..24}.exr; do sleep 0.35; cp "$f" "$WORK/" 2>/dev/null; done
Sleep 6
Key >
Sleep 1.5
Key <
Key Space
Sleep 3
Key G
Sleep 3
Key Space
Sleep 1
