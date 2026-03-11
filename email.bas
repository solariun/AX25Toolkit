10  REM =========================================================
20  REM  email.bas -- BBS Email System (SQLite-backed)
30  REM  Vars: callsign$, bbs_name$, db_path$
40  REM  Commands: LIST  READ <n>  COMPOSE <to> <subj>
50  REM            REPLY <n>  DELETE <n>  QUIT
60  REM =========================================================
70  DBOPEN db_path$
80  DBEXEC "CREATE TABLE IF NOT EXISTS msgs (id INTEGER PRIMARY KEY AUTOINCREMENT, from_call TEXT NOT NULL, to_call TEXT NOT NULL, subject TEXT DEFAULT '', body TEXT DEFAULT '', read INTEGER DEFAULT 0, ts DATETIME DEFAULT (datetime('now')))"
90  PRINT ""
100 PRINT "=== " + bbs_name$ + " Email System ==="
110 PRINT "User: " + callsign$
120 GOSUB 1000
130 PRINT "Commands: LIST  READ <n>  COMPOSE <to> <subj>  REPLY <n>  DELETE <n>  QUIT"
140 PRINT ""
150 REM ---- Main command loop ----
160 SEND "Email> "
170 RECV cmd$, 120000
180 IF cmd$ = "" THEN GOTO 160
190 LET kw$ = UPPER$(TRIM$(cmd$))
200 IF kw$ = "QUIT" OR kw$ = "Q" OR kw$ = "EXIT" OR kw$ = "BYE" THEN GOTO 900
210 IF LEFT$(kw$, 4) = "LIST" THEN GOSUB 1000
220 IF LEFT$(kw$, 4) = "READ" THEN GOSUB 2000
230 IF LEFT$(kw$, 4) = "COMP" THEN GOSUB 3000
240 IF LEFT$(kw$, 5) = "REPLY" THEN GOSUB 4000
250 IF LEFT$(kw$, 3) = "DEL" THEN GOSUB 5000
260 GOTO 160

900 REM ---- Exit ----
910 PRINT ""
920 PRINT "Goodbye from BBS Email!"
930 DBCLOSE
940 END

1000 REM ============================================================
1010 REM  LIST -- show messages addressed to this user
1020 REM ============================================================
1030 DBQUERY "SELECT COUNT(*) FROM msgs WHERE to_call='" + callsign$ + "'", tot$
1040 DBQUERY "SELECT COUNT(*) FROM msgs WHERE to_call='" + callsign$ + "' AND read=0", unr$
1050 PRINT ""
1060 PRINT "Messages for " + callsign$ + ": " + tot$ + " total, " + unr$ + " unread"
1070 IF tot$ = "0" THEN PRINT " (no messages)" : RETURN
1080 PRINT " ID  N  FROM         DATE              SUBJECT"
1090 PRINT "----+--+------------+-----------------+---------------------"
1100 DBFETCHALL "SELECT id, CASE WHEN read=0 THEN '*' ELSE ' ' END, from_call, substr(ts,1,16), subject FROM msgs WHERE to_call='" + callsign$ + "' ORDER BY id DESC LIMIT 20", rows$, "|", "~"
1110 IF rows$ = "" THEN RETURN
1120 LET pos% = 1
1130 LET rowlen% = LEN(rows$)
1140 WHILE pos% <= rowlen%
1150   LET tilde% = INSTR(MID$(rows$, pos%), "~")
1160   IF tilde% = -1 THEN LET rowdata$ = MID$(rows$, pos%) : LET pos% = rowlen% + 1
1170   IF tilde% <> -1 THEN LET rowdata$ = MID$(rows$, pos%, tilde% - 1) : LET pos% = pos% + tilde%
1180   LET p1% = INSTR(rowdata$, "|")
1190   IF p1% = -1 THEN GOTO 1290
1200   LET rid$ = LEFT$(rowdata$, p1% - 1)
1210   LET r1$ = MID$(rowdata$, p1% + 1)
1220   LET p2% = INSTR(r1$, "|")
1230   IF p2% = -1 THEN GOTO 1290
1240   LET flag$ = LEFT$(r1$, p2% - 1)
1250   LET r2$ = MID$(r1$, p2% + 1)
1260   LET p3% = INSTR(r2$, "|")
1270   IF p3% = -1 THEN GOTO 1290
1280   LET from$ = LEFT$(r2$, p3% - 1)
1285   LET r3$ = MID$(r2$, p3% + 1)
1286   LET p4% = INSTR(r3$, "|")
1287   IF p4% = -1 THEN LET ts$ = r3$ : LET subj$ = ""
1288   IF p4% <> -1 THEN LET ts$ = LEFT$(r3$, p4% - 1) : LET subj$ = MID$(r3$, p4% + 1)
1289   PRINT " " + LEFT$(rid$ + "   ", 3) + " " + flag$ + "  " + LEFT$(from$ + "            ", 12) + " " + LEFT$(ts$ + "                 ", 17) + " " + LEFT$(subj$ + "                     ", 21)
1290 WEND
1300 RETURN

2000 REM ============================================================
2010 REM  READ <id> -- display a message
2020 REM ============================================================
2030 LET sp% = INSTR(cmd$, " ")
2040 IF sp% = -1 THEN PRINT "Usage: READ <id>" : RETURN
2050 LET rid$ = TRIM$(MID$(cmd$, sp% + 1))
2060 IF rid$ = "" THEN PRINT "Usage: READ <id>" : RETURN
2070 DBQUERY "SELECT id FROM msgs WHERE id=" + rid$ + " AND to_call='" + callsign$ + "'", chk$
2080 IF chk$ = "" THEN PRINT "No message #" + rid$ + " for you." : RETURN
2090 DBQUERY "SELECT from_call FROM msgs WHERE id=" + rid$, mfrom$
2100 DBQUERY "SELECT ts        FROM msgs WHERE id=" + rid$, mts$
2110 DBQUERY "SELECT subject   FROM msgs WHERE id=" + rid$, msubj$
2120 DBQUERY "SELECT body      FROM msgs WHERE id=" + rid$, mbody$
2130 PRINT ""
2140 PRINT "--- Message #" + rid$ + " ---"
2150 PRINT "From   : " + mfrom$
2160 PRINT "Date   : " + mts$
2170 PRINT "Subject: " + msubj$
2180 PRINT "---"
2190 PRINT mbody$
2200 PRINT "---"
2210 DBEXEC "UPDATE msgs SET read=1 WHERE id=" + rid$
2220 RETURN

3000 REM ============================================================
3010 REM  COMPOSE <to> <subject> -- write a new message
3020 REM ============================================================
3030 LET csp1% = INSTR(cmd$, " ")
3040 IF csp1% = -1 THEN PRINT "Usage: COMPOSE <to> <subject>" : RETURN
3050 LET crest$ = TRIM$(MID$(cmd$, csp1% + 1))
3060 LET csp2% = INSTR(crest$, " ")
3070 IF csp2% = -1 THEN LET cto$ = UPPER$(crest$) : LET csubj$ = "(no subject)"
3080 IF csp2% <> -1 THEN LET cto$ = UPPER$(LEFT$(crest$, csp2% - 1)) : LET csubj$ = TRIM$(MID$(crest$, csp2% + 1))
3090 PRINT "Composing to " + cto$ + " / Subject: " + csubj$
3100 PRINT "Enter body (. alone to send, CANCEL to abort):"
3110 LET cbody$ = ""
3120 LET cdone% = 0
3130 LET cok% = 1
3140 WHILE cdone% = 0
3150   SEND "> "
3160   RECV cline$, 300000
3170   IF TRIM$(cline$) = "." THEN cdone% = 1
3180   IF UPPER$(TRIM$(cline$)) = "CANCEL" THEN cdone% = 1
3190   IF UPPER$(TRIM$(cline$)) = "CANCEL" THEN cok% = 0
3200   IF cdone% = 0 THEN cbody$ = cbody$ + cline$ + "\n"
3210 WEND
3220 IF cok% = 0 THEN PRINT "Cancelled." : RETURN
3230 DBEXEC "INSERT INTO msgs (from_call,to_call,subject,body) VALUES ('" + callsign$ + "','" + cto$ + "','" + csubj$ + "','" + cbody$ + "')"
3240 PRINT "Message sent to " + cto$ + "."
3250 RETURN

4000 REM ============================================================
4010 REM  REPLY <id> -- reply to a message
4020 REM ============================================================
4030 LET rsp% = INSTR(cmd$, " ")
4040 IF rsp% = -1 THEN PRINT "Usage: REPLY <id>" : RETURN
4050 LET rrid$ = TRIM$(MID$(cmd$, rsp% + 1))
4060 DBQUERY "SELECT from_call FROM msgs WHERE id=" + rrid$ + " AND to_call='" + callsign$ + "'", rto$
4070 IF rto$ = "" THEN PRINT "No message #" + rrid$ + " for you." : RETURN
4080 DBQUERY "SELECT subject FROM msgs WHERE id=" + rrid$, rosubj$
4090 LET cmd$ = "COMPOSE " + rto$ + " Re: " + rosubj$
4100 GOSUB 3000
4110 RETURN

5000 REM ============================================================
5010 REM  DELETE <id> -- delete a message you own
5020 REM ============================================================
5030 LET dsp% = INSTR(cmd$, " ")
5040 IF dsp% = -1 THEN PRINT "Usage: DELETE <id>" : RETURN
5050 LET drid$ = TRIM$(MID$(cmd$, dsp% + 1))
5060 DBQUERY "SELECT id FROM msgs WHERE id=" + drid$ + " AND to_call='" + callsign$ + "'", dchk$
5070 IF dchk$ = "" THEN PRINT "No message #" + drid$ + " for you." : RETURN
5080 PRINT "Delete message #" + drid$ + "? (Y/N)"
5090 RECV dconf$, 30000
5100 IF UPPER$(TRIM$(dconf$)) <> "Y" THEN PRINT "Cancelled." : RETURN
5110 DBEXEC "DELETE FROM msgs WHERE id=" + drid$
5120 PRINT "Message #" + drid$ + " deleted."
5130 RETURN
