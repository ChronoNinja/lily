###
SyntaxError: Variant Option::Some expects 1 args, but got 2.
Where: File "test/fail/wrong_arg_count_scoped_variant.ly" at line 11
###

enum class Option[A] {
	::Some(A),
	::None
}

Option[integer] opt = Option::Some(1, 2)
