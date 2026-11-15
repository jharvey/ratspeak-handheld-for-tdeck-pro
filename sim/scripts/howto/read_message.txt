# How-to video: read a message. Device ht_alex; companion_sam_send.txt (ht_sam) sends one.
expect 60000 [BOOT] ready
serial wiz skip
serial contact {SAM} SAM
wait 12000
key M
key H
wait 1500
serial idle now
wait 2500
rec read_message
caption The radio is asleep. A new message arrives...
expect 90000 [MSG] inbound
wait 2500
caption The idle screen shows the unread count at the top. Press any key to wake the radio.
wait 2500
key I
wait 2000
caption Press I for the Inbox. Chats with new messages are marked.
wait 1500
key I
wait 2200
caption Pick the chat (T / V, or SYM + its number) and press Enter.
wait 1200
key ENTER
wait 2500
caption The newest message is at the bottom. T / V scroll through older ones.
wait 4000
caption Type an answer and press Enter, or press ^ (Shift) to go back.
wait 1500
key BACK
wait 2000
rec off
quit
