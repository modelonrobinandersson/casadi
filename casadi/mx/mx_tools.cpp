/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "mx_tools.hpp"
#include "mapping.hpp"
#include "norm.hpp"
#include "mx_constant.hpp"
#include "if_else_node.hpp"
#include "../fx/mx_function.hpp"
#include "../matrix/matrix_tools.hpp"
#include "../stl_vector_tools.hpp"
#include "densification.hpp"

using namespace std;

namespace CasADi{

MX vertcat(const vector<MX>& comp){
  // Remove nulls
  vector<MX> c;
  c.reserve(comp.size());
  for(vector<MX>::const_iterator it=comp.begin(); it!=comp.end(); ++it)
    if(!it->isNull())
      c.push_back(*it);
  
  if(c.empty()){
    return MX();
  } else if(c.size()==1){
    return c[0];
  } else {
    // Construct the sparsity pattern
    CRSSparsity sp = c[0].sparsity();
    for(int i=1; i<c.size(); ++i){
      sp.append(c[i].sparsity());
    }

    // Create a mapping matrix with the corresponding sparsity
    MX ret;
    ret.assignNode(new Mapping(sp));
    
    // Map the dependencies
    int offset=0;
    for(int i=0; i<c.size(); ++i){
      int nz = c[i].size();
      ret->addDependency(c[i],range(nz),range(offset,offset+nz));
      offset += nz;
    }
    
    simplifyMapping(ret);
    return ret;
  }
}

MX horzcat(const vector<MX>& comp){
  vector<MX> v(comp.size());
  for(int i=0; i<v.size(); ++i)
    v[i] = trans(comp[i]);
  return trans(vertcat(v));
}

MX vertcat(const MX& a, const MX& b){
  vector<MX> ab;
  ab.push_back(a);
  ab.push_back(b);
  return vertcat(ab);
}

MX horzcat(const MX& a, const MX& b){
  vector<MX> ab;
  ab.push_back(a);
  ab.push_back(b);
  return horzcat(ab);
}

MX veccat(const vector<MX>& comp) {
  return vertcat(applymap(vec,comp));
}

MX vecNZcat(const vector<MX>& comp) {
  return vertcat(applymap(vecNZ,comp));
}

vector<MX> applymap(MX (*f)(const MX&) ,const vector<MX>& comp) {
  vector<MX> ret(comp.size());
  for (int k=0;k<comp.size();k++) {
    ret[k] = f(comp[k]);
  }
  return ret;
}

void applymap(void (*f)(MX&), vector<MX>& comp) {
  for (int k=0;k<comp.size();k++) {
    f(comp[k]);
  }
}

MX norm_2(const MX &x){
  MX ret;
  ret.assignNode(new Norm2(x));
  return ret;
}

MX norm_1(const MX &x){
  MX ret;
  ret.assignNode(new Norm1(x));
  return ret;
}

MX norm_inf(const MX &x){
  MX ret;
  ret.assignNode(new NormInf(x));
  return ret;
}

MX mul(const MX &x, const MX &y){
  return x.mul(y);
}

bool isZero(const MX& ex){
  if(ex.size()==0){
    return true;
  } else {
    const MXConstant* n = dynamic_cast<const MXConstant*>(ex.get());
    if(n==0){
      return false;
    } else {
      return isZero(n->x_);
    }
  }
}

bool isOne(const MX& ex){
  const MXConstant* n = dynamic_cast<const MXConstant*>(ex.get());
  if(n==0){
    return false;
  } else {
    return isOne(n->x_);
  }
}

bool isMinusOne(const MX& ex){
  const MXConstant* n = dynamic_cast<const MXConstant*>(ex.get());
  if(n==0){
    return false;
  } else {
    return isMinusOne(n->x_);
  }
}

bool isIdentity(const MX& ex){
  const MXConstant* n = dynamic_cast<const MXConstant*>(ex.get());
  if(n==0)
    return false;
  else
    return isIdentity(n->x_);
}

MX inner_prod(const MX &x, const MX &y){
  return x.inner_prod(y);
}

MX outer_prod(const MX &x, const MX &y){
  return x.outer_prod(y);
}

void simplifyMapping(MX& ex){
  return;
  
  // Make sure that we have a mapping with one dependency
  if(!(ex->isMapping() && ex->ndep()==1))
    return;
  
  // Check sparsity
  if(!(ex.sparsity() == ex->dep(0).sparsity()))
    return;
      
  // Get the mapping matrix nonzeros
  const vector<int>& nzmap = ex.mapping().data();
    
  // Check if the nonzeros follow in increasing order
  for(int el=0; el<nzmap.size(); ++el){
    if(el != nzmap[el]){
      return;
    }
  }
    
  // Identity transformation if we reached this point
  ex = ex->dep(0);
}

MX trans(const MX &x){
  // Quick return if null or scalar
  if(x.isNull() || x.numel()==1)
    return x;
  
  // Get the tranposed matrix and the corresponding mapping
  vector<int> nzind;
  CRSSparsity sp = x->sparsity().transpose(nzind);
  MX ret = MX::create(new Mapping(sp));
  ret->addDependency(x,nzind);
  
  // Check if the matrix is in fact an identity mapping (this will make sure that trans(trans(x)) -> x
  simplifyMapping(ret);
  
  return ret;
}

MX reshape(const MX &x, const std::vector<int> sz){
  if(sz.size() != 2)
    throw CasadiException("MX::reshape: not two dimensions");
  return reshape(x,sz[0],sz[1]);
}

MX reshape(const MX &x, int n, int m){
  if(n==x.size1() && m==x.size2())
    return x;
  else
    return reshape(x,x.sparsity().reshape(n,m));
}

MX reshape(const MX &x, const CRSSparsity& sp){
  // quick return if already the right shape
  if(sp==x.sparsity())
    return x;
  
  // make sure that the number of zeros agree
  casadi_assert(x.size()==sp.size());
  
  // Create a mapping
  MX ret = MX::create(new Mapping(sp));
  ret->addDependency(x,range(x.size()));
  
  // Simplify mapping if possible
  simplifyMapping(ret);
  
  return ret;
}

MX vec(const MX &x) {
  if(x.size2()==1){
    return x;
  } else {
    return reshape(trans(x),x.numel(),1);
  }
}

MX flatten(const MX &x) {
  if(x.size2()==1){
    return x;
  } else {
    return reshape(x,x.numel(),1);
  }
}

MX vecNZ(const MX &x) {
  // Create a mapping
  MX ret = MX::create(new Mapping(CRSSparsity(x.size(),1,true)));
  IMatrix ind(x.sparsity(),range(x.size()));
  ret->addDependency(x,trans(ind).data());
  simplifyMapping(ret);
  return ret;
}

MX if_else_zero(const MX &cond, const MX &if_true){
  MX ret;
  ret.assignNode(new IfNode(cond,if_true));
  return ret;
}


MX if_else(const MX &cond, const MX &if_true, const MX &if_false){
  return if_else_zero(cond,if_true) + if_else_zero(1-cond,if_false);
}

MX unite(const MX& A, const MX& B){
  // Join the sparsity patterns
  std::vector<unsigned char> mapping;
  CRSSparsity sp = A.sparsity().patternUnion(B.sparsity(),mapping);
  
  // Split up the mapping
  std::vector<int> nzA,nzB;
  
  // Copy sparsity
  for(int k=0; k<mapping.size(); ++k){
    if(mapping[k]==1){
      nzA.push_back(k);
    } else if(mapping[k]==2){
      nzB.push_back(k);
    } else {
      throw CasadiException("Pattern intersection not empty");
    }
  }
  
  // Create mapping
  MX ret;
  ret.assignNode(new Mapping(sp));
  ret->addDependency(A,range(nzA.size()),nzA);
  ret->addDependency(B,range(nzB.size()),nzB);
  simplifyMapping(ret);
  return ret;
}

bool isSymbolic(const MX& ex){
  if (ex.isNull())
    return false;
  return ex->isSymbolic();
}

MX trace(const MX& A){
  casadi_assert_message(A.size1() == A.size2(), "trace: must be square");
  MX res(0);
  for (int i=0; i < A.size1(); i ++) {
    res+=A(i,i);
  }
  return res;
}

MX repmat(const MX &A, int n, int m){
  // Quick return if possible
  if(n==1 &&  m==1)
    return A;
  
  // First concatenate horizontally
  MX row = horzcat(std::vector<MX >(m, A));
  
  // Then vertically
  return vertcat(std::vector<MX >(n, row));
}

/**
MX clip(const MX& A, const CRSSparsity& sp) {
  // Join the sparsity patterns
  std::vector<int> mapping;
  CRSSparsity sp = A.sparsity().patternIntersection(sp,mapping);
  
  // Split up the mapping
  std::vector<int> nzA,nzB;
  
  // Copy sparsity
  for(int k=0; k<mapping.size(); ++k){
    if(mapping[k]<0){
      nzA.push_back(k);
    } else if(mapping[k]>0){
      nzB.push_back(k);
    } else {
      throw CasadiException("Pattern intersection not empty");
    }
  }
  
  // Create mapping
  MX ret;
  ret.assignNode(new Mapping(sp));
  ret->addDependency(A,range(nzA.size()),nzA);
  ret->addDependency(B,range(nzB.size()),nzB);
  return ret;
  
}
*/

MX densify(const MX& x){
  MX ret = x;
  makeDense(ret);
  return ret;
}

void makeDense(MX& x){
  // Quick return if already dense
  if(x.dense()) return;
  
  // Densify
  x = MX::create(new Densification(x));
}

MX createParent(std::vector<MX> &deps) {
  // First check if arguments are symbolic
  for (int k=0;k<deps.size();k++) {
    if (!isSymbolic(deps[k])) throw CasadiException("createParent: the argumenst must be pure symbolic");
  }
  
  // Collect the sizes of the depenencies
  std::vector<int> index(deps.size()+1,0);
  for (int k=0;k<deps.size();k++) {
    index[k+1] =  index[k] + deps[k].size();
  }
  
  // Create the parent
  MX P("P",index[deps.size()],1);
  
  // Make the arguments dependent on the parent
  for (int k=0;k<deps.size();k++) {
    deps[k] = reshape(P(range(index[k],index[k+1])),deps[k].sparsity());
  }
  
  return P;
}

std::pair<MX, std::vector<MX> > createParent(const std::vector<CRSSparsity> &deps) {
  // Collect the sizes of the depenencies
  std::vector<int> index(deps.size()+1,0);
  for (int k=0;k<deps.size();k++) {
    index[k+1] =  index[k] + deps[k].size();
  }
  
  // Create the parent
  MX P("P",index[deps.size()],1);
  
  std::vector<MX> ret(deps.size());
  
  // Make the arguments dependent on the parent
  for (int k=0;k<deps.size();k++) {
    ret[k] =  reshape(P(range(index[k],index[k+1])),deps[k]);
  }
  
  return std::pair< MX, std::vector<MX> > (P,ret);
}

std::pair<MX, std::vector<MX> > createParent(const std::vector<MX> &deps) {
  std::vector<MX> ret(deps);
  MX P = createParent(ret);
  return std::pair< MX, std::vector<MX> > (P,ret);
}

MX operator==(const MX& a, const MX& b){
  casadi_assert_message(0,"Not implemented");
  return MX();
}

MX operator>=(const MX& a, const MX& b){
  casadi_assert_message(0,"Not implemented");
  return MX();
}

MX operator<=(const MX& a, const MX& b){
  casadi_assert_message(0,"Not implemented");
  return MX();
}

MX operator!(const MX& a){
  casadi_assert_message(0,"Not implemented");
  return MX();
}

MX diag(const MX& x){
  // Nonzero mapping
  std::vector<int> mapping;
  
  // Get the sparsity
  CRSSparsity sp = x.sparsity().diag(mapping);
  
  // Create a mapping
  MX ret = MX::create(new Mapping(sp));
  ret->addDependency(x,mapping);
  simplifyMapping(ret);
  return ret;
}

int countNodes(const MX& A){
  MXFunction f(vector<MX>(),A);
  f.init();
  return f.countNodes();
}

MX sumRows(const MX &x) {
  return mul(MX::ones(1,x.size1()),x);
}

MX sumCols(const MX &x) {
  return mul(x,MX::ones(x.size2(),1));
}

MX sumAll(const MX &x) {
  return sumCols(sumRows(x));
}


MX polyval(const MX& p, const MX& x){
  casadi_assert_message(isDense(p),"polynomial coefficients vector must be a vector");
  casadi_assert_message(isVector(p) && p.size()>0,"polynomial coefficients must be a vector");
  MX ret = p[0];
  for(int i=1; i<p.size(); ++i){
    ret = ret*x + p[i];
  }
  return ret;
}

bool isVector(const MX& ex){
  return ex.size2()==1;
}

bool isDense(const MX& ex){
  return ex.size() == ex.numel();
}

MX msym(const std::string& name, int n, int m){
  return MX(name,n,m);
}

MX msym(const std::string& name, const std::pair<int,int> & nm) {
  return MX(name,nm.first,nm.second);
}

MX msym(const Matrix<double>& x){
  return MX(x);
}

MX msym(const std::string& name, const CRSSparsity& sp) {
  return MX(name,sp);
}

bool isEqual(const MX& ex1,const MX &ex2){
  if ((ex1.size()!=0 || ex2.size()!=0) && (ex1.size1()!=ex2.size1() || ex1.size2()!=ex2.size2())) return false;
  MX difference = ex1 - ex2;  
  return isZero(difference);
}

std::string getOperatorRepresentation(const MX& x, const std::vector<std::string>& args) {
  //if (!x.isBinary()) throw CasadiException("getOperatorRepresentation: SX must be binary operator");
  if (x.isNorm()) {
    std::stringstream s;
    if (dynamic_cast<const Norm2*>(x.get())!=0) {
      dynamic_cast<const Norm2*>(x.get())->printPart(s,0);
      s << args[0];
      dynamic_cast<const Norm2*>(x.get())->printPart(s,1);
    } else  if (dynamic_cast<const NormF*>(x.get())!=0) {
      dynamic_cast<const NormF*>(x.get())->printPart(s,0);
      s << args[0];
      dynamic_cast<const NormF*>(x.get())->printPart(s,1);
    } else if (dynamic_cast<const Norm1*>(x.get())!=0) {
      dynamic_cast<const Norm1*>(x.get())->printPart(s,0);
      s << args[0];
      dynamic_cast<const Norm1*>(x.get())->printPart(s,1);
    } else if (dynamic_cast<const NormInf*>(x.get())!=0) {
      dynamic_cast<const NormInf*>(x.get())->printPart(s,0);
      s << args[0];
      dynamic_cast<const NormInf*>(x.get())->printPart(s,1);
    }
    return s.str();
  }

  
  if (args.size() == 0 || casadi_math<double>::ndeps(x.getOp())==2 && args.size() < 2) throw CasadiException("getOperatorRepresentation: not enough arguments supplied");
  std::stringstream s;
  casadi_math<double>::print(x.getOp(),s,args[0],args[1]);
  return s.str();
}


} // namespace CasADi

