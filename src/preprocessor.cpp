// Copyright (C) 2016 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <standardese/preprocessor.hpp>

#ifdef _MSC_VER
#pragma warning(push)
// 'sprintf' : format string '%ld' requires an argument of type 'long', but variadic argument 1 has type 'size_t'
#pragma warning(disable : 4477)
#endif

#include <boost/wave/cpplexer/cpp_lex_iterator.hpp>
#include <boost/wave/cpplexer/cpp_lex_token.hpp>
#include <boost/wave.hpp>
#include <boost/version.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if (BOOST_VERSION / 100000) != 1
#error "require Boost 1.x"
#endif

#if ((BOOST_VERSION / 100) % 1000) < 55
#warning "Boost less than 1.55 isn't tested"
#endif

#include <standardese/config.hpp>
#include <standardese/translation_unit.hpp>

using namespace standardese;
namespace bw = boost::wave;
namespace fs = boost::filesystem;

namespace
{
    using input_iterator = std::string::const_iterator;
    using token_iterator = bw::cpplexer::lex_iterator<bw::cpplexer::lex_token<>>;

    class policy : public bw::context_policies::default_preprocessing_hooks
    {
    public:
        policy(const preprocessor& pre, cpp_file& file, std::string& include)
        : pre_(&pre), file_(&file), include_(&include)
        {
        }

        template <typename ContextT, typename ContainerT>
        bool found_warning_directive(const ContextT&, const ContainerT&)
        {
            // ignore warnings
            return true;
        }

        template <typename ContextT>
        bool found_include_directive(const ContextT& ctx, std::string file_name, bool include_next)
        {
            bool is_system;
            if (use_include(ctx, file_name, is_system, include_next))
            {
                if (ctx.get_iteration_depth() == 0)
                    // in the main file
                    file_->add_entity(
                        cpp_inclusion_directive::make(*file_, file_name,
                                                      is_system ? cpp_inclusion_directive::system :
                                                                  cpp_inclusion_directive::local,
                                                      0));
                return false;
            }
            else
            {
                // write include so that libclang can use it
                *include_ += '#';
                *include_ += include_next ? "include_next" : "include";
                *include_ += is_system ? '<' : '"';
                *include_ += file_name;
                *include_ += is_system ? '>' : '"';
                *include_ += '\n';

                return true;
            }
        }

        template <typename ContextT, typename ParametersT, typename DefinitionT>
        void defined_macro(const ContextT& ctx, const bw::cpplexer::lex_token<>& name,
                           bool is_function_like, const ParametersT& parameters,
                           const DefinitionT& definition, bool is_predefined)
        {
            if (is_predefined || ctx.get_iteration_depth() != 0)
                // not in the main file
                return;

            std::string str_name = name.get_value().c_str();

            std::string str_params;
            if (is_function_like)
            {
                str_params += '(';
                for (auto& token : parameters)
                    str_params += token.get_value().c_str();
                str_params += ')';
            }

            std::string str_def;
            for (auto& token : definition)
                str_def += token.get_value().c_str();

            file_->add_entity(cpp_macro_definition::make(*file_, std::move(str_name),
                                                         std::move(str_params), std::move(str_def),
                                                         unsigned(name.get_position().get_line())));
        }

        template <typename ContextT>
        void undefined_macro(const ContextT& ctx, const bw::cpplexer::lex_token<>& name)
        {
            if (ctx.get_iteration_depth() != 0)
                // not in the main file
                return;

            auto prev = static_cast<cpp_entity*>(nullptr);
            for (auto& entity : *file_)
            {
                if (entity.get_name() == name.get_value().c_str())
                    break;
                prev = &entity;
            }

            file_->remove_entity_after(prev);
        }

    private:
        template <typename ContextT>
        bool use_include(const ContextT& ctx, std::string& file_name, bool& is_system,
                         bool include_next)
        {
            if (include_next)
                return false;

            assert(file_name[0] == '<' || file_name[0] == '"');
            is_system = file_name[0] == '<';

            file_name.erase(file_name.begin());
            file_name.pop_back();

            std::string dir;
            if (!ctx.find_include_file(file_name, dir, is_system, nullptr))
                return false;
            return pre_->is_preprocess_directory(fs::path(dir).remove_filename().generic_string());
        }

        const preprocessor* pre_;
        cpp_file*           file_;
        std::string*        include_;
    };

    using context = bw::context<input_iterator, token_iterator,
                                bw::iteration_context_policies::load_file_to_string, policy>;

    void setup_context(context& cont, const compile_config& c)
    {
        // set language to C++11 preprecessor
        // inserts additional whitespace to separate tokens
        // emits line directives
        // preserve comments
        auto lang = bw::support_cpp | bw::support_option_variadics | bw::support_option_long_long
                    | bw::support_option_insert_whitespace | bw::support_option_emit_line_directives
                    | bw::support_option_preserve_comments;
        cont.set_language(bw::language_support(lang));

        // add macros and include paths
        for (auto iter = c.begin(); iter != c.end(); ++iter)
        {
            if (*iter == "-D")
            {
                ++iter;
                cont.add_macro_definition(iter->c_str());
            }
            else if (*iter == "-U")
            {
                ++iter;
                cont.remove_macro_definition(iter->c_str());
            }
            else if (*iter == "-I")
            {
                ++iter;
                cont.add_sysinclude_path(iter->c_str());
            }
            else if (iter->c_str()[0] == '-')
            {
                if (iter->c_str()[1] == 'D')
                    cont.add_macro_definition(&(iter->c_str()[2]));
                else if (iter->c_str()[1] == 'U')
                    cont.remove_macro_definition(&(iter->c_str()[2]));
                else if (iter->c_str()[1] == 'I')
                    cont.add_include_path(&(iter->c_str()[2]));
            }
        }
    }
}

std::string preprocessor::preprocess(const compile_config& c, const char* full_path,
                                     const std::string& source, cpp_file& file) const
{
    std::string include;
    context     cont(source.begin(), source.end(), full_path, policy(*this, file, include));
    setup_context(cont, c);

    std::string preprocessed;
    for (auto& token : cont)
    {
        preprocessed += token.get_value().c_str();
        if (!include.empty())
        {
            preprocessed += include;
            include.clear();
        }
    }
    return preprocessed;
}
