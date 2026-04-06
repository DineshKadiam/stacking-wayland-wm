#!/bin/sh
# cow-title.sh -- emit waybar JSON for focused window title with minimum
# unambiguous ID prefix.  In per-output mode, only shows on the output
# the focused view lives on.

OUTPUT="${WAYBAR_OUTPUT_NAME:-}"

cowbar | while read -r line; do
    echo "$line" | jq -r --arg o "$OUTPUT" '
        .focused as $f |
        (.desktop_configuration // "global") as $mode |

        # In per-output mode, blank the title if the focused view is
        # on a different output than this bar instance.
        if $mode == "per-output" and ($f != null) and ($f.output != $o) then
            {"text": "", "tooltip": ""} | @json
        elif $f == null then
            {"text": "", "tooltip": ""} | @json
        else
            (.window_ids // []) as $ids |
            ($f.id // "") as $id |
            ($f.title // "") as $title |
            ($f.app_id // "") as $app |

            # Shortest prefix of $id, minimum 8 chars, unique among all IDs.
            (if $id == "" then ""
             else
               reduce range(8; ($id | length) + 1) as $len (
                 "";
                 if . != "" then .
                 else
                   ($id[0:$len]) as $prefix |
                   if ([$ids[] | select(. != $id and startswith($prefix))] | length) == 0
                   then $prefix
                   else ""
                   end
                 end
               )
             end) as $short_id |

            {
                text:    ("        [" + $short_id + "] " + $title + "        "),
                tooltip: ($app)
            } | @json
        end
    '
done
