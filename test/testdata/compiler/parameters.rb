# frozen_string_literal: true
# typed: true
# compiled: true

def foo(a, b=1, c:, d:2)
end

puts method(:foo).parameters

def noparam
end

puts method(:noparam).parameters
