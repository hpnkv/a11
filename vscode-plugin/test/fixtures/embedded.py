"""A flow inside a string, which is where most flows live."""

import a11.flow as flow

program = flow.loads("""
flow embedded {
  in  question: strig required
  out answer:   string

  question -> answer
}
""")
