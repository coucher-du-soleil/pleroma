#pragma once

#include <string>
#include <map>
#include <vector>
#include <stack>
#include <list>
#include "hylic_tokenizer.h"
#include "hylic_ast.h"


extern TokenStream tokenstream;

void load_file(std::string);
std::map<std::string, AstNode*> parse(TokenStream stream);
bool typecheck(std::map<std::string, AstNode*>);
AstNode *eval(AstNode *obj, Scope *scope);
void parse_file(std::string path);