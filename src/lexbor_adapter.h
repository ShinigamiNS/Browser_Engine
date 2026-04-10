// lexbor_adapter.h
#pragma once
#include "dom.h"
#include <string>
#include <memory>
#include <lexbor/html/html.h>

std::shared_ptr<Node> lexbor_parse_to_dom(const std::string& html);
std::string lexbor_extract_css(const std::shared_ptr<Node>& root);
