###
SyntaxError: Match pattern not exhaustive. The following case(s) are missing:
* None
Where: File "test/fail/match_incomplete.ly" at line 17
###

enum class Option[A] {
    Some(A),
    None
}

var v = None

match v: {
    case Some(s):
        print("Impossible!\n")
}
