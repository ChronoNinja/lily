list[integer] test = [1, 2, 3]
test.append(4)

define toapply(integer a => integer) { return a * 2 }

test.apply(toapply)
