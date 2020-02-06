# frozen_string_literal: true
# typed: strict

require 'set'
require 'optparse'
require_relative '../../../run/tools/preamble.rb'

class Module
  include T::Sig
end

module Intrinsics
  class Method < T::Struct
    prop :exported, T::Boolean
    prop :will_be_exported, T::Boolean
    prop :file, String
    prop :klass, String
    prop :rb_name, String
    prop :c_name, String
    prop :argc, Integer
  end

  class Edit < T::Struct
    prop :orig, String
    prop :edited, String
    prop :line, Integer
  end

  class Main

    SORBET_CLASSES = T.let(Set[
      "Array",
      "Complex",
      "Enumerable",
      "File",
      "Float",
      "Hash",
      "Integer",
      "NilClass",
      "Range",
      "String",
    ], T::Set[String])

    DIFF_WHITELIST = T.let(Set[
      'string.c',
      'array.c',
      'numeric.c',
    ], T::Set[String])

    sig {params(ruby: String, ruby_source: String).void}
    def self.run(ruby:, ruby_source:)
      puts "ruby binary: #{ruby}"
      puts "ruby source: #{ruby_source}"

      exported = exported_symbols(ruby: ruby)
      methods = methods_defined(ruby_source: ruby_source, exported: exported)

      File.open('intrinsic-report.md', 'w') do |report|
        puts 'Writing intrinsic-report.md'
        write_report(report, methods)
      end

      File.open('export-intrinsics.patch', 'w') do |diff|
        puts 'Writing export-intrinsics.patch'
        write_diff(ruby_source, diff, methods.filter {|k,_| DIFF_WHITELIST.include?(k)})
      end

      # group methods together
      grouped_methods = methods.values.flatten.group_by(&:c_name).values

      File.open('WrappedIntrinsics.h', 'w') do |header|
        puts 'Writing WrappedIntrinsics.h'
        write_header(header, grouped_methods)
      end

      File.open('PayloadIntrinsics.c', 'w') do |wrapper|
        puts 'Writing PayloadIntrinsics.c'
        write_wrapper(wrapper, grouped_methods)
      end
    end

    # Collect a set of exported symbols from the ruby binary, using 'nm'
    sig {params(ruby: String).returns(T::Set[String])}
    def self.exported_symbols(ruby:)
      exported = Set.new

      if !File.exist?(ruby)
        puts "Ruby binary is missing: #{ruby}"
        exit 1
      end

      IO.popen([ 'nm', ruby ]) do |io|
        io.each_line do |line|
          line = T.let(line, String)
          if line =~ / T /

            # the line output format for nm is '<addr> <type> <name>'
            symbol = line.split(' ')[2]

            # There ends up being leading underscores in front of all the
            # symbols we care about on macOS
            exported << T.must(symbol).delete_prefix('_')
          end
        end
      end

      exported
    end

    # Collect methods defined by file
    sig {params(ruby_source: String, exported: T::Set[String]).returns(T::Hash[String, T::Array[Method]])}
    def self.methods_defined(ruby_source:, exported:)
      defined = T.let({}, T::Hash[String, T::Array[Method]])

      # change to the source dir to avoid absolute paths in the output
      Dir.chdir(ruby_source) do
        files = T.let([], T::Array[String])
        IO.popen(
          [
            'find', '.',
            '-name', '*.c',
            '-not', '-path', '*/spec/*',
            '-not', '-path', '*/ext/*',
            '-not', '-path', '*/gems/*',
          ]) do |io|
            io.each_line do |file|
              file = T.let(file, String)
              # trim the trailing newline, and the leading './'
              files << T.must(file.chomp[2..])
            end
        end

        files.sort!

        files.each do |file|
          methods = methods_from(exported: exported, file: file)

          if !methods.empty?
            defined[file] = methods
          end
        end
      end

      defined
    end

    RB_DEFINE_CLASS = /([^\s]+)\s+=\s*rb_define_class\("([^"]+)/
    RB_DEFINE_MODULE = /([^\s]+)\s+=\s*rb_define_module\("([^"]+)/
    RB_DEFINE_CLASS_UNDER = /([^\s]+)\s+=\s*rb_define_class_under\([^,]+,\s*"([^"]+)/
    RB_DEFINE_MODULE_UNDER = /([^\s]+)\s+=\s*rb_define_module_under\([^,]+,\s*"([^"]+)/

    RB_DEFINE_METHOD = /rb_define_method\(([^,]+),\s*\"([^,]+)\",([^,]+),(.*)/

    # Collect methods defined in a single file.
    sig {params(exported: T::Set[String], file: String).returns(T::Array[Method])}
    def self.methods_from(exported:, file:)
      methods = T.let([], T::Array[Method])

      File.open(file, 'r') do |io|

        # Seed the class list with cases that are defined outside of the source
        # file that defines its methods.
        klasses = T.let({
          "rb_cArray" => "Array",
          "rb_cBasicObject" => "BasicObject",
          "rb_cFile" => "File",
          "rb_cFloat" => "Float",
          "rb_cIO" => "IO",
          "rb_cInteger" => "Integer",
          "rb_cModule" => "Module",
          "rb_cNilClass" => "NilClass",
          "rb_cNumeric" => "Numeric",
          "rb_cRange" => "Range",
          "rb_cString" => "String",
          "rb_cThread" => "Thread",
          "rb_eInterrupt" => "Interrupt",
          "rb_eSignal" => "SignalException",
          "rb_mEnumerable" => "Enumerable",
          "rb_mKernel" => "Kernel",
        }, T::Hash[String, String])

        io.each_line do |line|

          # This is a heuristic, but Init functions usually define a class as a
          # symbol, and thread that through the method definitions.
          klass_match =
            RB_DEFINE_CLASS.match(line) ||
            RB_DEFINE_MODULE.match(line) ||
            RB_DEFINE_CLASS_UNDER.match(line) ||
            RB_DEFINE_MODULE_UNDER.match(line)

          if klass_match
            match_1 = T.must(klass_match[1])
            match_2 = T.must(klass_match[2])
            klasses[match_1.chomp] = match_2
          end

          if m = RB_DEFINE_METHOD.match(line)
            m1, m2, m3, m4 = T.must(m[1]), T.must(m[2]), T.must(m[3]), T.must(m[4])
            klass = m1.chomp

            # It would be nice to log a message here if the class can't be
            # resolved, but in most cases they're classes we're not interested
            # in.
            if klass_value = klasses[klass]
              rb_name = m2
              c_name = m3.lstrip.rstrip
              argc = m4.chomp.to_i

              methods << Method.new(
                exported: exported.include?(c_name),
                will_be_exported: false,
                file: file,
                klass: klass_value,
                rb_name: rb_name,
                c_name: c_name,
                argc: argc
              )
            end
          end
        end
      end

      methods
    end

    sig {params(report: File, files: T::Hash[String, T::Array[Method]]).void}
    def self.write_report(report, files)
      total_intrinsics = 0
      exported_intrinsics = 0

      report << "# Intrinsics by file\n"
      report << "\n"

      files.each do |file,methods|
        report << "## `#{file}`\n"

        methods.each do |method|

          total_intrinsics += 1

          exported = ' '
          if method.exported
            exported_intrinsics += 1
            exported = 'x'
          end

          report << "* [#{exported}] `#{method.c_name}` (`#{method.klass}##{method.rb_name}`)\n"
        end

        report << "\n"
      end

      report << "## Stats\n"
      report << "* Total:   #{total_intrinsics}\n"
      report << "* Visible: #{exported_intrinsics}\n"
    end

    sig do
      params(
        ruby_source: String,
        diff: File,
        methods: T::Hash[String, T::Array[Method]]
      )
        .void
    end
    def self.write_diff(ruby_source, diff, methods)
      Dir.chdir(ruby_source) do
        methods.each do |file,methods|
          hidden = methods.filter {|m| !m.exported}
          if !hidden.empty?
            edits = File.open(file, 'r') do |io|
              expose_methods(hidden, io)
            end

            if !edits.empty?
              diff_header(diff, file)
              edits.each do |edit|
                diff_chunk(diff, edit)
              end
            end
          end
        end
      end
    end

    sig {params(io: File, file: String).void}
    def self.diff_header(io, file)
      io << "--- #{file}\n"
      io << "+++ #{file}\n"
    end

    sig {params(io: File, edit: Edit).void}
    def self.diff_chunk(io, edit)
      io << "@@ -#{edit.line} +#{edit.line} @@\n"
      io << "-#{edit.orig}"
      io << "+#{edit.edited}"
    end

    sig {params(hidden: T.untyped, io: File).returns(T::Array[Edit])}
    def self.expose_methods(hidden, io)
      edits = T.let([], T::Array[Edit])

      previous = ''
      io.each_line.each_with_index do |line,idx|
        hidden.each_with_index do |method|
          # idx is 0-based
          edit = generate_edit(previous, line, idx+1, method)
          if !edit.nil?
            edits << edit
            method.will_be_exported = true
            break
          end
        end

        previous = line
      end

      edits
    end

    # Heuristic for finding a method definition given a line and the previous
    # line for context.
    sig do
      params(
        previous: String,
        line: String,
        idx: Integer,
        method: Method
      )
        .returns(T.nilable(Intrinsics::Edit))
    end
    def self.generate_edit(previous, line, idx, method)
      if !line.match?(method.c_name)
        return nil
      end

      if previous.match?('^static VALUE$')
        return Edit.new(
          orig: previous,
          edited: previous.sub('static ', ''),
          line: idx - 1
        )
      end

      if line.match?('static VALUE')
        return Edit.new(
          orig: line,
          edited: line.sub('static ', ''),
          line: idx
        )
      end

      nil
    end


    sig {params(method: Method).returns(T.nilable(T::Boolean))}
    def self.should_wrap?(method)
      (method.exported || method.will_be_exported) && SORBET_CLASSES.include?(method.klass)
    end

    sig {params(header: File, grouped_methods: T::Array[T::Array[Method]]).void}
    def self.write_header(header, grouped_methods)
      header << "// This file is autogenerated. Do not edit it by hand. Regenerate it with:\n"
      header << "//   cd compiler/IREmitter/Intrinsics && make\n"

      grouped_methods.each do |methods|
        method = methods.fetch(0)
        if should_wrap? method
          header << "    {core::Symbols::#{method.klass}(), "
          header << "\"#{method.rb_name}\", "
          header << "\"sorbet_int_#{method.c_name}\", Intrinsics::HandleBlock::Unhandled},\n"
        end
      end
    end

    sig {params(wrapper: File, grouped_methods: T::Array[T::Array[Method]]).void}
    def self.write_wrapper(wrapper, grouped_methods)

      wrapper << <<~EOF
      #ifndef SORBET_LLVM_IMPORTED_INTRINSICS_H
      #define SORBET_LLVM_IMPORTED_INTRINSICS_H

      // This file is autogenerated. Do not edit it by hand. Regenerate it with:
      //   cd compiler/IREmitter/Intrinsics && make

      #include "ruby.h"

      typedef VALUE (*BlockFFIType)(VALUE firstYieldedArg, VALUE closure, int argCount, VALUE *args, VALUE blockArg);

      EOF

      grouped_methods.each do |methods|
        method = methods.fetch(0)
        if should_wrap? method

          # TODO: comment about the ruby method
          wrapper << "\n"
          forward_declare_intrinsic(wrapper, method, methods)
          wrapper << "\n"
          wrapper_implementation(wrapper, method)
        end
      end

      wrapper << "#endif /* SORBET_LLVM_IMPORTED_INTRINSICS_H */\n"
    end

    sig {params(wrapper: File, method: Method, methods: T::Array[Method]).void}
    def self.forward_declare_intrinsic(wrapper, method, methods)

      methods.each do |m|
        wrapper  << "// #{m.klass}##{m.rb_name}\n"
      end
      wrapper << "// Calling convention: #{method.argc}\n"

      wrapper << "extern VALUE #{method.c_name}("

      case method.argc
      when -1
        wrapper << 'int argc, const VALUE *args, VALUE obj'
      else
        args = ['VALUE obj']

        method.argc.times do |i|
          args << "VALUE arg_#{i}"
        end

        wrapper << args.join(', ')
      end

      wrapper << ");\n"
    end

    sig {params(wrapper: File, method: Method).void}
    def self.wrapper_implementation(wrapper, method)

      # NOTE: the sorbet calling convention differs from -1, in that we put the
      # receiver first, and pass the ID of the fun explicitly.
      wrapper << "VALUE sorbet_int_#{method.c_name}("
      wrapper << 'VALUE recv, '
      wrapper << 'ID fun, '
      wrapper << 'int argc, '
      wrapper << "VALUE *const restrict args, BlockFFIType blk, VALUE closure) {\n"

      case method.argc

      when -1
        # dispatch directly to the underlying intrinsic
        wrapper << "    return #{method.c_name}(argc, args, recv);\n"

      when -2
        puts "Need to implement calling convention -2"
        exit 1

      else
        # check the arity
        wrapper << "    rb_check_arity(argc, #{method.argc}, #{method.argc});\n"

        # extract individual arguments
        args = ["recv"]
        method.argc.times do |i|
          args << "arg_#{i}"
          wrapper << "    VALUE arg_#{i} = args[#{i}];\n"
        end

        # call the underlying intrinsic
        wrapper << "    return #{method.c_name}(#{args.join(', ')});\n"
      end

      wrapper << "}\n"
    end

  end
end

if __FILE__ == $0

  topdir = File.dirname($0) + '/../../..'

  ruby = topdir + '/bazel-bin/external/sorbet_ruby_unpatched/toolchain/bin/ruby'

  ruby_source = topdir + '/bazel-sorbet_llvm/external/sorbet_ruby_unpatched'

  OptionParser.new do |opts|

    opts.banner = "Usage: wrap-intrinsics.rb [options]"

    opts.on '-rPATH', '--ruby=PATH', 'Path to the ruby executable to analyze' do |path|
      ruby = File.realpath(path)
    end

    opts.on '-sPATH', '--ruby-source=PATH', 'Path to the ruby source to analyze' do |path|
      ruby_source = File.realpath(path)
    end

    opts.on '-h', '--help', 'Display this message' do
      puts opts
      exit
    end

  end.parse!

  Intrinsics::Main.run(ruby: ruby, ruby_source: ruby_source)
end
