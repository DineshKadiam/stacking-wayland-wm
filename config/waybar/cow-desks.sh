#!/bin/sh
# cow-desks.sh -- emit waybar JSON for cow desktop state
# Called by waybar with $WAYBAR_OUTPUT_NAME set to this bar's output.

OUTPUT="${WAYBAR_OUTPUT_NAME:-}"

cowbar | while read -r line; do
    echo "$line" | jq -r --arg o "$OUTPUT" '
        ((.outputs // []) | map(select(.name == $o)) | first) as $out |
        if $out == null then empty else
            ($out.desks // []) as $desks |

            # Only active and occupied desks shown; empty desks skipped.
            # Separated by | in magenta #FF00FF.
            ([
                $desks[] |
                if .active then
                    "<span background=\"#39c488\" color=\"#ffffff\"> " + (.nr|tostring) + " </span>"
                elif .collected then
                    "<span background=\"#cc6600\" color=\"#ffffff\"> " + (.nr|tostring) + " </span>"
                elif .windows > 0 then
                    "<span background=\"#004C98\" color=\"#ffffff\"> " + (.nr|tostring) + " </span>"
                else
                    empty
                end
            ] | join("<span color=\"#808080\">|</span>")) as $spans |

            ([$desks[] | select(.active)] | first | .windows // 0) as $active_wins |

            # Info block: yellow-green background, white text, magenta leading pipe
            ("<span color=\"#FF00FF\">|</span>"
              + "<span background=\"#E3A3BE\" color=\"#ffffff\">"
              + "["     + ($out.page_col|tostring) + ", " + ($out.page_row|tostring) + "]</span>"
              + "<span background=\"#D7C72F\" color=\"#ffffff\">"
              + "[Scr:" + ($out.name // "") + "]"
              + "[A:"   + ($active_wins | tostring) + "]"
              + "[L:"   + (.desktop_configuration // "") + "]"
              + "</span>") as $info |

            {
                text:    ($spans + $info),
                tooltip: ("desk " + ($out.current_desk | tostring))
            } | @json
        end
    '
done
