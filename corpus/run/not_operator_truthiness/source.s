def probe():
  ok_basic = (not true) == false and (not false) == true and (not null) == true
  ok_truthy = (not 0) == false and (not "") == false and (not [1]) == false
  ok_expr = (not (1 == 2)) == true and (4.even? and not 4.odd?)
  ok_comparison_precedence = not Int === "wrong" and not 1 == 2
  x = 0
  unless false:
    x = 1
  unless true:
    x = 2
  ok_unless = x == 1
  if ok_basic and ok_truthy and ok_expr and ok_comparison_precedence and ok_unless:
    42
  else:
    0
