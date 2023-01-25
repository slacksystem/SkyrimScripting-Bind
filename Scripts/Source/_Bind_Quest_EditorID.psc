scriptName _Bind_Quest_EditorID extends Quest
{!BIND MQ101}

event OnInit()
    ; If this will be an 0xFF dynamic form, do not print out `self` (it't not deterministic, which we want for tests)
    string script = StringUtil.Substring(self, 1, StringUtil.Find(self, " ") - 1)
    Debug.Trace("[!BIND] Script " + script + " bound to " + GetID())
endEvent
